# mw-streamer

`mw-streamer` 集成了一份固定版本的轻量 ZLMediaKit 源码，用于后续构建可嵌入业务进程的流媒体静态库。

当前协议核心保留：

- RTMP、RTSP、SRT、HTTP-FLV、HTTP-TS、HLS 拉流。
- RTMP、RTSP、SRT 推流。
- MP4、FMP4、FLV、MPEG-TS、MPEG-PS 封装与解封装能力。
- Linux 和 Windows 网络运行时。
- 可选 OpenSSL；未检测到 OpenSSL 时仍编译普通非 TLS 协议。

服务端主程序、Web API、WebRTC、语言绑定、移动端工程和上游测试未保留。当前源码不依赖 JSON。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

构建产物均为静态库。ZLMediaKit 固定源码位于 `third_party/ZLMediaKit`，其上游版本和裁剪边界记录在该目录的文档及 Git 历史中。

SRT reactor 是进程级资源。宿主退出时应先停止创建和重连 SRT 会话，在各会话所属
`EventPoller` 上完成 `teardown()` 并执行队列屏障，最后调用 `mw::shutdown()`。
该调用只会关闭已经创建的 SRT reactor，不会在退出阶段反向创建它；关闭过程会等待
reactor 线程释放 SRT epoll 并在同一线程执行 `srt_cleanup()`。关闭后不可再次创建
SRT 会话。

SRT 每次新建或重连发布会丢弃关键帧之前的残缺历史数据，并从包含 PAT、PMT 和随机访问点的完整 TS 关键帧批次开始发送，避免高码率流从 GOP 中段接入时无法完成接收端初始化。

## 日志

日志模块使用一个活动的 spdlog logger 统一接收 `mw-streamer`、ZLMediaKit、libsrt 和
FFmpeg 日志，并在正文前分别增加 `[streamer]`、`[ZLM]`、`[SRT]` 和
`[FFMPEG]`。各模块级别、控制台、滚动文件及异步队列通过 `mw::log::LogConfig`
配置；异步日志默认关闭，彩色控制台与普通控制台不会同时创建。

```cpp
#include <mw/init.hpp>

int main() {
    using Log = mw::log::Module<mw::log::LogModule::Streamer>;

    // init 前使用懒加载的默认同步控制台 logger。
    Log::info("program started");

    mw::InitConfig config;
    config.log.modules.zlm = mw::log::LogLevel::Info;
    config.log.modules.srt = mw::log::LogLevel::Info;
    config.log.modules.ffmpeg = mw::log::LogLevel::Warning;
    mw::init(config);

    // 创建并使用媒体对象。

    // 先停止所有媒体线程和第三方回调，再关闭全局模块。
    mw::shutdown();

    // shutdown 后再次回到默认 logger。
    Log::info("program stopped");
}
```

`mw::shutdown()` 必须由宿主控制线程调用，不能从 SRT reactor 或媒体回调线程调用。
`mw::init()` 使用一次性初始化：第一次成功调用的配置生效，后续调用不会替换配置；
`mw::shutdown()` 后不支持重新初始化。初始化构造失败不会消耗这次机会，可以修正配置
后再次调用。
动态库场景必须在 `dlclose()` 或 `FreeLibrary()` 前完成媒体对象销毁和
`mw::shutdown()`，不能把该操作放入 `DllMain`。
mw-streamer 在 init 到 shutdown 期间独占 ZLM、libsrt 和 FFmpeg 的全局日志接入；
宿主不要同时替换这些全局回调。shutdown 后 libsrt 恢复 warning 等级、完整原生格式
和默认输出回调，FFmpeg 恢复默认回调及 init 前的日志等级。

日志模块也允许用户直接持有 `mw::log::Logging`，其作用域负责接管和释放日志桥接。
手动持有与 `mw::init()` 是两种互斥的所有权方式，同一进程同时只能存在一个活动的
`Logging`。手动模式下 `mw::initialized()` 仍表示 init 模块未启动，用户应在媒体
线程停止后自行销毁 `Logging`；`mw::shutdown()` 不会销毁这份外部对象。若使用了
SRT，应在 `Logging` 仍存活时先调用 `mw::shutdown()` 关闭 Reactor，再销毁它。

默认格式为
`[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v`，其中 `%t` 是线程 ID，
`%v` 是包含模块前缀的正文。未调用 `mw::init()` 时，Streamer 日志使用首次写入时
创建的默认同步控制台 logger；调用 `mw::init()` 后由 init 模块持有配置后的
`Logging`，并接管 ZLM、SRT 和 FFmpeg 日志。`mw::shutdown()` 会解除接管、排空
配置后的日志后端并恢复默认日志路径。
