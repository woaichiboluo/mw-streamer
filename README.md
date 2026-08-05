# mw-streamer

`mw-streamer` 集成了一份固定版本的轻量 ZLMediaKit 源码，用于后续构建可嵌入业务进程的流媒体静态库。

当前协议核心保留：

- RTMP、RTSP、SRT、HTTP-FLV、HTTP-TS、HLS 拉流。
- RTMP、RTSP、SRT 推流。
- MP4、FMP4、FLV、MPEG-TS、MPEG-PS 封装与解封装能力。
- Linux 和 Windows 网络运行时。
- FFmpeg、SRT 和 OpenSSL 由用户环境提供，项目不内置这些依赖。

服务端主程序、Web API、WebRTC、语言绑定、移动端工程和上游测试未保留。当前源码不依赖 JSON。

## 构建

```bash
# C ABI交付模式，默认生成so或dll，只导出mw_*接口。
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON
cmake --build build-shared --parallel

# C++集成模式，生成包含Pipeline公开API的静态库。
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build-static --parallel
```

两种模式互斥，一个构建目录只生成一种 `mw-streamer` 库，默认使用 `SHARED`。
动态模式只公开 `mw/c_api.h` 中的 C ABI；静态模式供 C++ 调用方直接使用 Pipeline
等公开类型。FFmpeg、SRT 和 OpenSSL 在两种模式下都由用户预先安装；CMake 按平台
默认顺序优先查找共享库，也接受用户提供的静态库，不对依赖的许可证策略做判断。
ZLMediaKit 固定源码位于
`third_party/ZLMediaKit`，其上游版本和裁剪边界记录在该目录的文档及 Git 历史中。

测试和示例由独立选项控制，与静态库或动态库模式无关：

```bash
cmake -S . -B build \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`BUILD_TESTS` 在项目作为顶层工程时默认开启，作为子工程时默认关闭；
`BUILD_EXAMPLES` 默认关闭。动态库模式下，C API 测试直接链接生成的动态库，内部 C++
测试则使用同源测试支撑库，因此不会扩大动态库的公开 ABI。

SRT reactor 是进程级资源。宿主退出时应先停止创建和重连 SRT 会话，在各会话所属
`EventPoller` 上完成 `teardown()` 并执行队列屏障，最后调用
`mw::streamer::Shutdown()`。
该调用只会关闭已经创建的 SRT reactor，不会在退出阶段反向创建它；关闭过程会等待
reactor 线程释放 SRT epoll 并在同一线程执行 `srt_cleanup()`。关闭后不可再次创建
SRT 会话。

SRT 每次新建或重连发布会丢弃关键帧之前的残缺历史数据，并从包含 PAT、PMT 和随机访问点的完整 TS 关键帧批次开始发送，避免高码率流从 GOP 中段接入时无法完成接收端初始化。

## TOML 配置

初始化配置、StreamingPipeline、RemuxPipeline 和 FilePipeline 均可从独立的
TOML 文件加载。未填写的字段沿用 C++ 配置默认值；未知字段、错误类型、超出目标
C++ 类型表示范围的整数和未知枚举值会在加载时直接报错，具体组件的组合约束则在
启动时校验。以毫秒为单位的字段使用 `_ms` 后缀。

```toml
input_url = "rtsp://127.0.0.1/live/input"
cache_duration_ms = 1000
output_targets = ["rtmp://127.0.0.1/live/output"]

[video_decoder]
backend = "cuda"

[processor]
output_width = 1920
output_height = 1080

[processor.config]
mode = "auto_ptz"

[processor.config.ai]
model = "detector.engine"

[video_encoder]
codec = "h264"
frame_rate = { num = 25, den = 1 }
```

`processor.config` 是开放的嵌套表，库不会解释其中字段，而是把该子树序列化成一份
独立 TOML 文本传给 Processor。C++ 调用方可通过 `mw/config/toml.h` 中的
`LoadInitConfigFromToml()`、`LoadStreamingPipelineConfigFromToml()`、
`LoadRemuxPipelineConfigFromToml()` 和 `LoadFilePipelineConfigFromToml()` 加载配置。
配置仍然只需要一个文件；`processor` 下由 Pipeline 使用的固定字段和允许动态更新的
`processor.config` 通过表层级区分，不需要调用方自行拆分或解析。

## C 接口

`mw/c_api.h` 提供三类 Pipeline 的纯 C 句柄接口。`start` 只表示异步启动请求被接受；
运行期失败通过状态回调或状态查询返回。统计快照由调用方使用对应的
`mw_*_stats_destroy()` 释放，错误详情由同一线程上的 `mw_last_error()` 获取。

```c
#include <mw/c_api.h>

MwStreaming* pipeline = NULL;
if (mw_init(NULL) == kMwResultSuccess &&
    mw_streaming_create("streaming.toml", &pipeline) == kMwResultSuccess) {
  mw_streaming_start(pipeline);
  // 修改原TOML后，只重新加载并应用processor.config子表。
  mw_streaming_reload(pipeline);
  mw_streaming_stop(pipeline);
  mw_streaming_destroy(pipeline);
}
mw_shutdown();
```

`mw_streaming_create()` 和 `mw_file_create()` 会保存创建时配置文件的绝对路径。
修改同一个 TOML 文件后，调用 `mw_streaming_reload()` 或 `mw_file_reload()` 即可重读
完整文件并只更新 `processor.config`；解析失败不会覆盖上一份有效配置。重新加载期间
生成的 `const char*` 只在 Processor 的 `update_config` 回调期间有效，需要异步使用时
由 Processor 自行复制。

## 日志

日志模块使用一个活动的 spdlog logger 统一接收 `mw-streamer`、Processor、
ZLMediaKit、libsrt 和 FFmpeg 日志，并在正文前分别增加 `[streamer]`、
`[processor]`、`[ZLM]`、`[SRT]` 和 `[FFMPEG]`。各模块级别、控制台、滚动文件及异步队列通过
`mw::streamer::log::LogConfig`
配置；异步日志默认关闭，彩色控制台与普通控制台不会同时创建。

```cpp
#include <mw/init/init.h>

int main() {
    using Log =
        mw::streamer::log::Module<mw::streamer::log::LogModule::kStreamer>;

    // init 前使用懒加载的默认同步控制台 logger。
    Log::Info("program started");

    mw::streamer::InitConfig config;
    config.log.modules.processor = mw::streamer::log::LogLevel::kInfo;
    config.log.modules.zlm = mw::streamer::log::LogLevel::kInfo;
    config.log.modules.srt = mw::streamer::log::LogLevel::kInfo;
    config.log.modules.ffmpeg = mw::streamer::log::LogLevel::kWarning;
    config.zlm.event_poller_threads = 4;
    config.zlm.work_threads = 2;
    config.zlm.enable_cpu_affinity = true;
    mw::streamer::Init(config);

    // 创建并使用媒体对象。

    // 先停止所有媒体线程和第三方回调，再关闭全局模块。
    mw::streamer::Shutdown();

    // shutdown 后再次回到默认 logger。
    Log::Info("program stopped");
}
```

ZLM线程池配置只在首次创建线程池前生效，因此必须先调用
`mw::streamer::Init()`，再创建任何Player、PacketQueue、Output或Pipeline。
线程数为0时由ZLToolKit按照硬件并发数决定。

`mw::streamer::Shutdown()` 必须由宿主控制线程调用，不能从 SRT reactor
或媒体回调线程调用。`mw::streamer::Init()` 使用一次性初始化：第一次成功调用的
配置生效，后续调用不会替换配置；`mw::streamer::Shutdown()` 后不支持重新初始化。
初始化构造失败不会消耗这次机会，可以修正配置后再次调用。
动态库场景必须在 `dlclose()` 或 `FreeLibrary()` 前完成媒体对象销毁和
`mw::streamer::Shutdown()`，不能把该操作放入 `DllMain`。
mw-streamer 在 init 到 shutdown 期间独占 ZLM、libsrt 和 FFmpeg 的全局日志接入；
宿主不要同时替换这些全局回调。shutdown 后 libsrt 恢复 warning 等级、完整原生格式
和默认输出回调，FFmpeg 恢复默认回调及 init 前的日志等级。

日志模块也允许用户直接持有 `mw::streamer::log::Logging`，其作用域负责接管和
释放日志桥接。手动持有与 `mw::streamer::Init()` 是两种互斥的所有权方式，
同一进程同时只能存在一个活动的 `Logging`。手动模式下
`mw::streamer::IsInitialized()` 仍表示 init 模块未启动，用户应在媒体线程停止后
自行销毁 `Logging`；`mw::streamer::Shutdown()` 不会销毁这份外部对象。若使用了
SRT，应在 `Logging` 仍存活时先调用 `mw::streamer::Shutdown()` 关闭 Reactor，
再销毁它。

默认格式为
`[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v`，其中 `%t` 是线程 ID，
`%v` 是包含模块前缀的正文。未调用 `mw::streamer::Init()` 时，Streamer 日志
使用首次写入时创建的默认同步控制台 logger；调用 `mw::streamer::Init()` 后由
init 模块持有配置后的 `Logging`，并接管 ZLM、SRT 和 FFmpeg 日志。
`mw::streamer::Shutdown()` 会解除接管、排空配置后的日志后端并恢复默认日志路径。
