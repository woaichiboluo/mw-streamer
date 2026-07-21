# mw-streamer

`mw-streamer` 是一个基于 FFmpeg 的 C++17 流媒体静态库。它嵌入调用方进程，负责输入、
压缩包延迟缓存、NVIDIA 硬件编解码、多路同步、同步业务 Processor、备播切换，以及一次
编码后的多目标输出；它不是 RTSP、RTMP 或 SRT 流媒体服务器。

## 已实现能力

### 协议与编码矩阵

| 方向 | 协议 | 说明 |
| --- | --- | --- |
| 输入 | RTMP | 直播客户端拉流 |
| 输入 | RTSP | 客户端拉流，支持显式 TCP 或 UDP，默认 TCP |
| 输入 | SRT | caller 模式客户端拉流 |
| 输入 | FILE | 有限文件输入，正常 EOF 后 Pipeline 完成 |
| 输入 | HLS | 调用方显式选择 live 或 VOD 模式 |
| 输出 | RTMP | H.264 使用标准 RTMP；H.265 自动启用增强 RTMP |
| 输出 | RTSP | 向已有 RTSP 服务器发布，支持显式 TCP 或 UDP |
| 输出 | SRT | caller/live 模式推送 MPEG-TS |
| 输出 | FILE | 仅 fragmented MP4 录像，不覆盖已有文件 |

不支持 HLS 输出，也不在库内启动任何协议服务器。网络 URL、鉴权参数和 SRT `streamid`
由调用方提供；额外 FFmpeg 协议选项可以通过输入、输出配置中的 `options` 透传。

视频输入和输出仅支持 H.264、H.265：

- 视频解码仅使用 NVIDIA CUVID/NVDEC。
- 视频编码仅使用 NVIDIA NVENC，支持 CBR/VBR、码率、GOP、B 帧数和 NVENC preset。
- 解码帧和 Processor 输出帧固定为 NVIDIA device 上的 8-bit NV12。
- 不提供 CPU 视频编解码、像素格式转换或其他 GPU 后端。

音频输入使用 FFmpeg 自动选择解码器，随后统一转换为 48 kHz、双声道、float32
interleaved，以 1024-frame 块交给 Processor。输出固定编码为 AAC-LC。所有输入均无音轨
时，不创建音频处理链路，也不生成输出音轨。

### Pipeline 行为

一条 Pipeline 包含一个或多个固定输入、一次视频编码、可选的一次音频编码，以及一个或
多个独立输出。主要行为如下：

- 以主视频输入为时钟，按微秒 PTS 对齐 N 路视频；缺少任一路时丢弃整组，不复用旧帧。
- 音频输入位置与视频输入位置一致；缺失音频补静音，不自动选择主音频或混音。
- 视频和音频编码包通过引用扇出到各输出端的独立有界队列。
- 单个输出阻塞、断开或失败不会阻塞编码器、Processor 和其他输出。
- 实时输入及网络输出按配置独立重连；`max_retries = 0` 表示无限重试。
- 直播 packet、已解码视频和归一化音频队列满时丢弃最旧数据；FILE/HLS VOD 使用背压且不主动丢帧。
- 实时输入可在解码前缓存压缩 `AVPacket`，通过 `live_delay` 形成固定延迟。
- 完整同步视频中断超过 `standby_timeout` 后输出备播图；恢复首组完整视频后立即回到业务画面。
- JPG/PNG 备播图在启动时加载、拉伸到输出尺寸并上传一次 GPU，运行期复用。
- 输出视频 PTS 由框架维护，业务画面、重连和备播切换期间保持单调连续。

## 构建

### 环境要求

- CMake 3.19 或更高版本。
- C++17 编译器。
- Linux x86_64：GCC 11 或更高版本。
- Windows x86_64：Visual Studio 2022 / MSVC 19.3x 或更高版本。
- 调用方预先编译并提供 FFmpeg 开发文件。
- 运行时存在可用的 NVIDIA GPU 和驱动。

FFmpeg 至少需要 `avcodec`、`avformat`、`avutil`、`swresample` 和 `swscale`，并启用项目使用的
RTMP、RTSP、SRT、HLS、CUDA hwcontext、`h264_cuvid`、`hevc_cuvid`、`h264_nvenc` 和
`hevc_nvenc` 能力。项目不会下载或编译 FFmpeg。

核心库不查找 CUDA Toolkit、不包含 CUDA 头文件，也不直接链接 `cudart`；NVDEC、NVENC
和 GPU frame 均通过 FFmpeg 硬件上下文使用。只有 `mw_streamer_cuda_processor_example`
需要 CUDA Toolkit，找不到 Toolkit 时 CMake 会跳过该示例，其他目标仍可构建。

### FFmpeg 查找

项目内的 `FindFFmpeg.cmake` 使用 pkg-config 结果作为搜索提示，再通过 CMake 的
`find_path()` 和 `find_library()` 确认头文件和库。假设预编译 FFmpeg 的根目录由
`MW_FFMPEG_ROOT` 指定：

```bash
export MW_FFMPEG_ROOT=/path/to/ffmpeg
export PKG_CONFIG_PATH=${MW_FFMPEG_ROOT}/lib/pkgconfig:${PKG_CONFIG_PATH}
export LD_LIBRARY_PATH=${MW_FFMPEG_ROOT}/lib:${LD_LIBRARY_PATH}

cmake -S . -B build \
  -DMW_STREAMER_BUILD_EXAMPLES=ON \
  -DMW_STREAMER_BUILD_TESTS=ON
cmake --build build --parallel
```

`LD_LIBRARY_PATH` 只在 FFmpeg 使用动态库且不在系统运行时搜索路径时需要。Windows 上应让
pkg-config 能找到 FFmpeg `.pc` 文件；使用动态 FFmpeg 时，还需让对应 DLL 位于 `PATH`
或程序目录中。也可以通过标准 CMake 搜索路径提供 FFmpeg 头文件和库。

fmt、spdlog 和启用测试时的 GoogleTest 使用 CMake `FetchContent` 获取，并锁定明确版本。
首次配置需要能够访问这些依赖的 Git 仓库，或事先配置 FetchContent 使用本地依赖源码。

### 作为子项目使用

顶层构建时 examples 和 tests 默认开启；通过 `add_subdirectory()` 或 `FetchContent` 作为
子项目使用时默认关闭。上层工程可以在加入项目之前显式设置：

```cmake
set(MW_STREAMER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MW_STREAMER_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(path/to/mw-streamer)
target_link_libraries(your_target PRIVATE mw::streamer)
```

当前仓库提供静态 target `mw_streamer` 和别名 `mw::streamer`，尚未提供安装、导出或
`find_package(mw_streamer)` 包。

## Pipeline 配置与生命周期

公开 API 位于 `include/mw/streamer/`。以下是使用默认 Processor 的最小 FILE 配置；默认
视频回调生成黑帧，默认音频回调生成静音，因此真实业务应按下一节安装 Processor callback：

```cpp
#include <utility>

#include "mw/streamer/pipeline.h"

int main() {
  mw::streamer::PipelineConfig config;
  config.id = "example";
  config.device_id = 0;

  mw::streamer::InputConfig input;
  input.id = "input-0";
  input.protocol = mw::streamer::InputProtocol::kFile;
  input.url = "input.mp4";
  config.inputs.push_back(std::move(input));

  mw::streamer::OutputConfig output;
  output.id = "recording";
  output.protocol = mw::streamer::OutputProtocol::kFile;
  output.url = "output.mp4";  // 必须尚不存在。
  config.outputs.push_back(std::move(output));

  config.video_encoder.codec = mw::streamer::VideoCodec::kH264;
  config.video_encoder.width = 1920;
  config.video_encoder.height = 1080;
  config.video_encoder.frame_rate = {25, 1};
  config.video_encoder.bit_rate = 4'000'000;
  config.video_encoder.gop_size = 50;
  config.standby_image_path = "standby.png";

  mw::streamer::Pipeline pipeline(std::move(config));
  mw::streamer::Status status = pipeline.Start();
  if (status.ok()) {
    status = pipeline.Wait();
  }
  return status.ok() ? 0 : 1;
}
```

关键配置包括：

- `inputs`、`outputs`：启动前固定，端点 `id` 必须全局唯一。
- `device_id`：一条 Pipeline 的解码、编码和所有 GPU frame 使用同一设备。
- `video_encoder`：固定输出尺寸、帧率、codec、码率、GOP、rate control 和 NVENC 选项。
- `synchronization`：主输入索引、PTS 容差和最大同步等待。
- `queues`：解码视频队列与归一化音频 FIFO 容量。
- `live_delay`：全部输入都是实时输入时可启用的压缩包固定延迟。
- `standby_timeout`、`standby_image_path`：完整视频中断检测和备播图。
- `processor_callbacks`、`processor_initial_config`：业务回调和不透明初始配置字符串。
- `callbacks`：Pipeline 状态及带稳定端点 ID 的重连、恢复、失败通知。

`Start()` 完成配置检查、输入流信息发现、GPU/Processor/编码器初始化和可用输出启动后返回。
`Wait()` 等待 `Completed`、`Failed` 或 `Stopped`；`Stop()` 同步且幂等，并取消阻塞 I/O 和
重连等待。Pipeline 是一次性对象，停止或完成后不能再次启动。生命周期接口只应由一个
外部控制线程调用。

单个输出启动失败只通过端点事件通知，不会阻止其他输出或整条 Pipeline 运行。网络输入
重试耗尽后保持备播和静音，等待调用方处理；FILE 或 HLS VOD 正常 EOF 则使有限 Pipeline
完成。

## Processor 回调

`include/mw/streamer/processor.h` 是 C 兼容头文件。Processor 边界只使用普通结构体、函数
指针和无类型 GPU 地址，不暴露 FFmpeg 或 CUDA 类型。每条 Pipeline 独占一份
`MwProcessorCallbacks`：

- `on_start`：接收固定的输入源信息、输出规格、`device_id` 和初始配置字符串。
- `on_video`：接收严格同步的 N 路只读 NV12 input view 和一块可写 NV12 output view。
- `on_audio`：接收对齐后的 N 路 48 kHz 双声道块和唯一可写输出块。
- `on_config_update`：处理 `Pipeline::UpdateProcessorConfig()` 传入的不透明字符串。
- `on_stop`：释放用户资源；返回后框架不再触发任何 Processor callback。

未设置 `on_video` 时框架把输出填为黑帧；未设置 `on_audio` 时填为静音。未设置其他回调
时分别视为成功或无操作。视频和音频 callback 在不同线程执行，配置更新也可以与它们并发；
用户负责自己的状态同步。

GPU frame view 中的 `y`、`uv` 是 `device_id` 对应设备上的 NV12 地址，并分别携带 pitch。
用户可以使用自己的 CUDA stream、TensorRT 或其他 GPU 库，但必须自行声明依赖，并在
callback 返回前完成所有输入读取和输出写入。框架不管理用户 stream，也不处理跨 GPU
拷贝。视频输入 view 暴露原始 PTS、time base 和微秒 PTS；输出时间戳字段由框架忽略。

音频 view 使用 frame-major interleaved float32，固定 1024 frames、48 kHz、双声道。框架
不混音；业务 callback 选择、复制或处理需要的输入。视频进入备播时跳过音频 callback 并
直接编码静音。

`on_start` 和 `on_config_update` 返回非零表示失败，可在框架提供的 512 字节缓冲区写入错误
消息。配置更新失败不会改变 Pipeline 状态，业务应保留上一份有效配置。所有 callback 都
不得让异常跨越 C ABI；`on_video`、`on_audio` 必须同步、实时并完整产生输出。

## HLS live 与 VOD

HLS 不自动推断运行模式，调用方必须明确设置：

```cpp
mw::streamer::InputConfig input;
input.protocol = mw::streamer::InputProtocol::kHls;
input.hls_mode = mw::streamer::HlsMode::kLive;  // 或 kVod
input.url = "https://media.example/live/index.m3u8";
```

- `kLive`：实时队列满时丢弃最旧数据；异常或 EOF 按重连策略处理；可以参与
  `live_delay`。
- `kVod`：队列满时阻塞以保持内容完整；读到 `EXT-X-ENDLIST` 后正常完成；不重连、不进入
  `live_delay`。

非 HLS 输入必须保留默认 `HlsMode::kLive`。只要配置了 `live_delay > 0`，全部输入都必须是
RTMP、RTSP、SRT 或 HLS live；live 与有限输入混合时会在启动校验阶段拒绝固定延迟。延迟
缓存要求参与调度的压缩包具有有效 DTS。

## Examples

构建 examples 后，可执行文件位于 `build/examples/`。

查看库版本：

```bash
./build/examples/mw_streamer_version_example
```

`mw_streamer_transcode_example` 展示协议解析、Pipeline 生命周期、HLS 模式和多输出配置。
它使用默认 Processor，所以视频为黑帧、音频为静音，不用于验证业务画面直拷：

```bash
./build/examples/mw_streamer_transcode_example \
  file input.mp4 output.mp4 standby.png \
  1920 1080 25 0 h264 0 0 file

./build/examples/mw_streamer_transcode_example \
  hls-live https://media.example/live/index.m3u8 \
  rtmp://media.example/live/output standby.png \
  1920 1080 25 60 h264 30000 0 rtmp \
  file recording.mp4

./build/examples/mw_streamer_transcode_example \
  hls-vod https://media.example/vod/index.m3u8 output.mp4 standby.png \
  1920 1080 25 0 h264 0 0 file
```

参数中的 `run-seconds` 为 `0` 时等待有限输入 EOF；直播示例应给出运行秒数，或由调用方
自行触发 `Stop()`。第一个输出协议在可选参数中指定，之后可以继续追加
`<output-protocol> <output-url>` 对。

如果 CMake 找到 CUDA Toolkit，还会构建 `mw_streamer_cuda_processor_example`。它展示用户
维护 CUDA stream、NV12 device-to-device copy、callback 返回前同步，以及音频选择：

```bash
# 单路原尺寸直拷。
./build/examples/mw_streamer_cuda_processor_example \
  identity output.mp4 standby.png 160 144 5 input.mp4 h264

# 两路等宽输入上下拼接；输出高度等于两个输入高度之和。
./build/examples/mw_streamer_cuda_processor_example \
  vstack output.mp4 standby.png 160 288 5 left.mp4 right.mp4 h264

# 单路网络输入，一次编码后同时推流和录像。
./build/examples/mw_streamer_cuda_processor_example \
  stream-identity rtsp-tcp rtsp://127.0.0.1:8554/input \
  standby.png 1920 1080 25 60 h264 \
  rtmp rtmp://127.0.0.1/live/output file recording.mp4
```

identity 模式要求输出尺寸与输入一致；vstack 要求两路输入等宽，且输出尺寸严格匹配拼接
结果。RTSP 和 SRT 输出都是连接已有服务器的发布客户端。

## 测试

完整验证命令：

```bash
cmake -S . -B build \
  -DMW_STREAMER_BUILD_TESTS=ON \
  -DMW_STREAMER_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

测试分为不依赖 GPU 的单元测试、`file-integration` 和 `gpu-integration`：

```bash
ctest --test-dir build -L file-integration --output-on-failure
ctest --test-dir build -L gpu-integration --output-on-failure
```

没有可用 NVIDIA device 时，需要真实 GPU 执行的测试通过 `GTEST_SKIP()` 明确跳过，而不是
伪造通过。FFmpeg 是否包含 CUDA hwcontext 以及四个必需 CUVID/NVENC codec 属于构建能力
检查；缺失这些能力会使相应测试失败，不会被当成“无 GPU”跳过。

## 当前限制

- 仅支持 C++17、x86_64 Linux/Windows 和 NVIDIA GPU；无 CPU fallback。
- 每个输入 URL 最多选择一路视频和一路音频；视频轨必需。
- 输入视频必须能够由 CUVID/NVDEC 产出 GPU NV12；不转换其他硬件像素格式。
- 输入分辨率、输出分辨率、输出帧率和端点列表在启动后固定。
- 不自动缩放业务输入、不插帧、不抽帧，也不修正用户配置的编码帧率。
- 多路同步不复用旧帧；任一必需视频源缺失时整组不可用并最终进入备播。
- 音频不自动选主路、不混音、不变速；这些逻辑由 Processor 决定。
- FILE 输出仅支持 fragmented MP4，已有目标文件会启动失败；不支持覆盖或追加。
- 不支持 HLS 输出，不提供 RTSP/RTMP/SRT 服务端，不实现 SRT listener/rendezvous。
- Pipeline 是一次性对象，运行时不能增删输入、输出或替换备播图。
- 当前没有完整的 C Pipeline API；只有 Processor callback 边界是 C 兼容接口。
- 当前没有安装、导出和稳定 ABI 包装；应通过 CMake 子项目方式链接。

项目日志统一写入 `spdlog::default_logger()`，不会修改调用进程的 logger/sink 配置。FFmpeg
日志经桥接后进入同一 logger。
