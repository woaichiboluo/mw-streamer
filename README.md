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
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_LINKAGE=SHARED \
  -DSRT_LINKAGE=SHARED
cmake --build build-static --parallel
```

项目交付供 C++ 宿主使用的静态库，公开接口位于 `include/mw/`，按模块组织。
Processor 的 callback、context 和 frame view 保留纯 C 兼容结构体与函数指针；
Pipeline 的 C 句柄封装暂不提供。FFmpeg、SRT 和 OpenSSL 由用户预先安装。FFmpeg 和 SRT
分别通过 `FFMPEG_LINKAGE`、`SRT_LINKAGE` 明确选择 `SHARED` 或 `STATIC`，不能根据
Windows 的 `.lib` 后缀推断；必要时可用 `FFmpeg_ROOT`、`SRT_ROOT` 指定安装目录。
构建还需要 CUDA Toolkit 头文件，可用 `CUDAToolkit_ROOT` 指定安装目录；库在使用
CUDA 帧时动态加载 NVIDIA 驱动，不链接 CUDA Runtime。
FFmpeg 要求安装 `pkg-config` 或兼容的 `pkgconf`，并为所用组件提供 `.pc` 文件；
静态 FFmpeg 的私有依赖由各组件的 `.pc` 传递。静态 SRT 的私有链接依赖由
`SRT::SRT` 根据匹配的 `srt.pc` 或 Windows 官方包中的 `libsrt.props` 传递。
ZLMediaKit 固定源码位于
`third_party/ZLMediaKit`，其上游版本和裁剪边界记录在该目录的文档及 Git 历史中。

测试和示例由独立选项控制：

```bash
cmake -S . -B build \
  -DFFMPEG_LINKAGE=SHARED \
  -DSRT_LINKAGE=SHARED \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`BUILD_TESTS` 在项目作为顶层工程时默认开启，作为子工程时默认关闭；
`BUILD_EXAMPLES` 默认关闭。

SRT reactor 是进程级资源。宿主退出时应先停止创建和重连 SRT 会话，在各会话所属
`EventPoller` 上完成 `teardown()` 并执行队列屏障，最后调用
`mw::streamer::Shutdown()`。
该调用只会关闭已经创建的 SRT reactor，不会在退出阶段反向创建它；关闭过程会等待
reactor 线程释放 SRT epoll 并在同一线程执行 `srt_cleanup()`。关闭后不可再次创建
SRT 会话。

SRT 每次新建或重连发布会丢弃关键帧之前的残缺历史数据，并从包含 PAT、PMT 和随机访问点的完整 TS 关键帧批次开始发送，避免高码率流从 GOP 中段接入时无法完成接收端初始化。

## Pipeline：Input 与统一 Sink

公开头文件与实现按职责分层，命名空间均位于 `mw::streamer` 下：

| 模块及命名空间 | 公开头文件位置 | 职责 |
|---|---|---|
| `pipeline` | `mw/pipeline/` | Pipeline、Builder 和图配置；组装节点、路由消息并协调生命周期 |
| `input` | `mw/input/` | Input、FileInput、ZlmInput、输入状态及 PlayerProxy；获取源数据 |
| `sink` | `mw/sink/` | 统一 Sink、SinkMessage、FatalError 和节点媒体类型、消费状态 |
| `media` | `mw/media/` | Packet、Frame 的投递参数、轨道就绪与时间线事件；纯 C 媒体类型 |
| `cache` | `mw/cache/` | PacketQueue 的压缩包缓存和调度 |
| `decoder` | `mw/decoder/` | DecoderSink、音视频解码器；输出 Frame |
| `processor` | `mw/processor/` | AnalysisProcessorSink、TransformProcessorSink 及业务回调适配 |
| `synchronizer` | `mw/synchronizer/` | SynchronizerSink；内部实时调度、同步和备播 |
| `encoder` | `mw/encoder/` | EncoderSink、音视频编码器；输出 Packet |
| `output` | `mw/output/` | RemuxSink、单目标转封装、推流和录像 |

各节点的参数结构放在所属模块的 `config.h`；`mw/pipeline/pipeline_config.h` 只描述
Input、节点 ID、媒体连接和消息路由，并复用这些参数。`ffmpeg`、`common`、
`performance` 分别提供媒体资源封装、线程容器工具和性能统计。

依赖由组装层指向组件：Pipeline 依赖 Input 和 Sink，Builder 负责选择具体实现；
Input 和 Sink 共享 `media` 的投递契约，具体处理节点依赖 Sink 及自身媒体能力。
PacketQueue 依赖 Sink 作为消费者。组件不依赖 Pipeline 的实现，Fatal 通过注入
的回调上报；消息投递同样由 Pipeline 注入。同步调度器和备播实现属于
`synchronizer/internal`，Processor 的共享回调上下文属于 `processor/internal`。

下面说明各接口的运行契约：

- `Input` 负责获取源数据，通过借用的 `Input::Observer` 同步调用对应的通知接口。
- `Sink` 是统一的链路节点，通过 `SinkMediaType` 声明输入与输出是 Packet、Frame
  或无输出。媒体与边界使用明确的虚函数，不使用 variant。基类独占持有下游，
  `AddSink()` 校验媒体类型；具体 Sink 自己负责媒体队列、调度和错误处理。
  每个 Sink 构造时必须传入非空 ID，通过 `id()` 读取，构造后不可更改。Pipeline
  启动前校验整棵树的 ID 唯一性，包括未配置消息路由的节点。
- `ZlmInput` 是基于 `PlayerProxy` 的输入实现，内部独占 PlayerProxy，将其媒体和
  控制回调转换为通用事件；每路输入使用独占的 Poller，重连复用同一实例。
  通用 `Input` 不依赖 ZLM 类型。
- `FileInput` 使用 FFmpeg 全速读取本地文件，不按播放时钟限速；保留原始时间戳
  和 Packet side data。它交付离线模式，DecoderSink 使用有界等待保证不主动丢包。
- `Pipeline` 独占持有一个 Input 和多个 Sink，实现 Observer 并按 Sink
  注册顺序同步转发四类通知。连接状态、错误和重试信息通过 `input_status()` 查询。

通过构造函数将 `std::unique_ptr<Input>` 交给 Pipeline，通过 `AddSink()`
转移各个 `std::unique_ptr<Sink>` 的所有权。Input 只借用 Pipeline 的 Observer。
各方法同步调用，参数只在调用期间借用；异步 Sink 自己保存所需数据，轨道信息
独立持有，Packet 保留引用，共享媒体数据只读。队列的内部结构由具体 Sink 决定。
阻塞 Sink 方法会阻塞输入和后续 Sink。

`EventPollerPool::extractPoller()` 从公共池移除尚未对外发放的 Poller，供调用方
独占使用。公共池只剩一个实例，或其余实例均已对外发放时，新建独占 Poller，
公共池始终保留至少一个实例。普通获取和遍历接口不会返回已提取的实例，即使
从独占 Poller 的线程调用 `getPoller()` 也是如此。独占实例释放后销毁，不归还池；
返回值沿用 ZLM 的 `EventPoller::Ptr`，调用方应仅在该输入的内部链路传递。
因此输入投递阻塞时，RemuxSink 所用的公共 Poller 仍可继续消费输出队列。

每个接口使用专用参数结构：`StreamsReady`、`PacketReady`、`TimelineReset` 和
`StreamEnded`。每个代次先交付轨道，再交付 Packet，最后结束；重连在新轨道前
通知时间线重置。结束原因明确区分 EOF、断流、主动停止和失败，无需业务 Sink
查询底层 PlayerProxy。连接、重试和错误状态通过 Observer 的
`OnInputStateChanged()` 更新 Pipeline 的状态快照；尚未得到轨道的失败只报告该状态通知。
`Input::state()` 和具体 Sink 的 `state()` 分别表示输入和消费端状态；输入 EOF
并不表示 Sink 已经排空或完成输出。

`ZlmInput::Start(observer)` 每个实例只允许启动一次，参数错误同步抛出，运行期
输入错误通过事件报告。`Stop()` 幂等，等待在途事件投递结束并解除 Observer 借用，
返回后不会继续投递；销毁 Observer 前必须先 Stop。Start、Stop 和输入析构必须在
输入执行线程之外调用，避免等待自身；Observer 回调内可以查询状态。Sink 的排空和
销毁由组合方另外管理。

使用 Pipeline 时，调用方只需提供具体 Sink：

```cpp
#include <memory>
#include <utility>

#include "mw/input/zlm_input.h"
#include "mw/pipeline/pipeline.h"
#include "mw/sink/sink.h"

namespace pipeline = mw::streamer::pipeline;
namespace input = mw::streamer::input;
namespace sinks = mw::streamer::sink;

std::unique_ptr<pipeline::Pipeline> StartInput(
    input::ZlmInputConfig config,
    std::unique_ptr<sinks::Sink> sink) {
  auto result = std::make_unique<pipeline::Pipeline>(
      std::make_unique<input::ZlmInput>(std::move(config)));
  result->AddSink(std::move(sink));
  result->Start();
  return result;
}
```

Pipeline 启动前必须至少注册一个 Sink；启动或停止后不能再添加 Sink，也不能重新
启动。`Start()` 的同步异常会停止 Input 和已注册 Sink 后向调用方传播。`Stop()`
幂等，先关闭消息投递并等待在途回调，向 Sink 调用 `RequestStop()` 解除阻塞投递，
再停止输入，随后调用各 Sink 的 `Stop()` 等待内部工作退出；媒体队列清理
策略由 Sink 决定。对象仍由 Pipeline 持有，析构时先销毁 Input，再销毁 Sink。
控制方法相互串行，Sink 回调内不得调用控制方法或销毁 Pipeline，可以查询输入状态。
`input_status()` 保存最近收到的状态通知，可能晚于正在投递的媒体边界；Sink 直接
使用 `OnInputEnded()` 的原因判断如何处理当前代次。

Pipeline 在 Start 时创建一个仅等待 fatal 停机请求的控制线程，不承载媒体处理，
不创建媒体队列，也不轮询 Sink 状态。解码线程由下面的 DecoderSink 持有。
`FileInput` 适合完整处理文件；`ZlmInput` 的本地文件播放仍按实时节奏推进。
文件 EOF 后应等待消费端排空，再调用 Pipeline 的 `Stop()`。音频、视频处理回调
可以并发；目前性能快照提供节点吞吐与耗时，不提供文件进度或媒体处理倍速。

### RemuxSink：单目标转推与录像

`RemuxSink` 继承 `Sink`，消费 Packet 且无媒体输出，一个实例只配置一个 `target`：RTMP、RTSP、SRT
推流地址，或 `.mp4` / `.m3u8` 录像路径。多个目标通过 Pipeline 注册多个独占
Sink 实现，各自管理状态、封装和关闭。

```cpp
#include "mw/output/remux_sink.h"

namespace output = mw::streamer::output;

// chain 是已创建、尚未启动的 pipeline::Pipeline。
output::RemuxSinkConfig recording;
recording.target = "/recordings/camera.mp4";
chain.AddSink(
    std::make_unique<output::RemuxSink>("recording", std::move(recording)));

output::RemuxSinkConfig publishing;
publishing.target = "rtsp://127.0.0.1:8554/live/camera";
chain.AddSink(
    std::make_unique<output::RemuxSink>("publishing", std::move(publishing)));
```

Sink 自行从 ZLToolKit 单例池取得并固定使用一个 Poller，不额外创建 Worker。
公开方法同步复制参数/引用并投递到 Sink 自有 BlockingQueue；Poller 按批次消费，
不会在输入调用中执行文件写入。`packet_queue_capacity` 默认 384，分别限制待处理
Packet 队列和启动缓存，控制事件不占该限额。启动缓存等待各轨道首包和编码参数，
以确定音视频共同的时间戳起点；超限明确报错，不丢旧包继续运行。
多个 Sink 可能共享池中的同一 Poller。

源编码参数和时间基跨代次保持稳定。正常输入重连复用输出，PTS 可以重排；任意
轨道 DTS 回退则关闭旧输出、清空各轨道时间戳历史并创建新输出。
录像创建带新时间戳的文件/目录，快速重建不会覆盖上一段；网络目标重新
建立发布会话。H264/H265 输入采用 Annex B，与 ZlmInput 的 Packet 格式一致。
录像保留收到的编码样本，包括首关键帧之前的音频、非关键视频和 AAC 的负时间戳
首包；不做解码或重新编码。音视频统一平移到共同起点，时间基转换为毫秒后保留
PTS/DTS 间隔，不进行逐轨归零或大间隔平滑。EOF 写完启动缓存和 HLS 尾部分片。
缺少必要编码参数或无法转换编码包时明确报错，不静默跳过。
网络输出保留 ZLM 的实时 GOP 缓存及重连策略，不保证补发连接建立之前的全部包。

输入暂时中断时保留输出；EOF 按顺序排空并完成收尾后进入 `kEnded`。`Stop()`
关闭投递入口、排空已接收工作并关闭输出，返回后无回调借用 Sink；必须在本 Sink
的 Poller 之外调用。网络暂时失败由 ZLM 重试；队列满、无效数据或目标永久失败
仅使该 Sink 进入 `kFailed`，通过 `error()` 查询原因。其他目标继续工作，显式
`FatalError` 仍使用统一的 Pipeline 停机通道。单目标网络状态可通过
`GetNetworkOutputSnapshot()` 查询，返回定义在
`mw/performance/pipeline_snapshot.h` 中的 `performance::NetworkOutputSnapshot`。

可运行示例：`mw_remux_sink_example input_url target [target...]`，每个 target
创建一个独立 RemuxSink；有限输入 EOF 后等待输出完成，Ctrl+C 有序停止。

## DecoderSink：Packet 到 Frame

`DecoderSink` 直接继承 `Sink`，消费 Packet 并输出 Frame，
通过 `AddSink(std::unique_ptr<Sink>)` 注册多个独占的 Frame 消费者。
它接收上述四类输入通知。
实时输入使用 `cache::PacketQueue`：队列首次投递时启动一个调度线程，音视频分开缓存，
共用缓存时长、播放时钟和输入代次。队列用条件等待完成定时调度，不依赖 Poller，
也不向 DecoderSink 暴露执行器。四类输入通知同步入队，由队列线程有序处理。
到期的 Packet 再进入各自的解码工作队列，每条实际存在的轨道使用一个 Worker；
纯音频或纯视频输入只创建对应的解码线程；实时缓存调度线程独立存在。
实时输入投递只保留数据并唤醒队列，`cache_duration = 0` 时也由队列线程直接转交。
离线输入绕过定时缓存，直接进入音频、视频的有界解码队列，不创建缓存调度线程。

PacketQueue 直接借用消费 Packet 的 `Sink` 作为出队消费端，沿用其四类通知接口。
EOF 和断流会排空缓存，停止和失败会丢弃缓存，随后仅向消费端投递一次
`OnInputEnded()`；新时间线会打断旧的定时等待并清除旧缓存。队列的
`state()`、`generation()` 和 `error()` 提供自身处理状态：`kDraining` 表示正在
排空缓存，`kEnded` 仅表示队列已完成交付，不代表下游解码完成。
DecoderSink 的外层接口接收入队，内部消费者处理到期包；队列排空后才安排
解码器结束。在显式 Stop 时先等待队列线程退出，再停止两个解码支路。
队列内部复用 `common::BlockingQueue`。

音频在 DecoderSink 内解码并重采样，输出固定为 48 kHz、float32 交错格式，
保持源声道布局，时间基为 `1/48000`。视频保持所选解码器的原始帧格式，CUDA
解码输出仍在 GPU 上；投递前等待源 CUDA 流完成写入，业务可在自己的 context 和
stream 中读取。`DecoderSinkConfig` 复用已有软解/CUDA 配置，不自动切换后端。
解码工作队列统一使用 `common::BlockingQueue`，按条件限流只计算 Packet，
重置和结束等控制消息不占限额。实时输入队列满时，音频丢弃当前包；视频清除
排队的 Packet，等待下一个关键帧并刷新解码器后恢复。生命周期控制消息不会随
Packet 被删除。FileInput 的离线模式使用有界等待把反压传回文件读取线程，不主动
丢弃音视频包，也不允许设置非零 `cache_duration`。

消费 Frame 的 Sink 通过 `OnAudioFrame()` 和 `OnVideoFrame()` 同步接收 `FrameReady`，
同一轨道有序，音视频可以并发。实现若需异步保存帧，复制参数以保留引用，共享
媒体缓冲区保持只读。`OnStreamsReady()` 携带原始压缩源信息和借用的硬件上下文；
实际帧的格式、尺寸与采样率以 Frame 为准，硬件上下文借用持续到 Stop 完成。

首次输入投递前至少注册一个消费 Frame 的 Sink；注册操作不得与输入、Stop 或其他注册
并发。空指针注册抛出 `invalid_argument`，首次输入通知或 Stop 后注册抛出
`logic_error`。没有消费者时，首次轨道配置会使 DecoderSink 进入 `kFailed`。
每次帧和边界通知按注册顺序同步分发，消费者共享只读媒体缓冲区；慢消费者会
阻塞对应轨道后续的投递。某个消费者抛出异常会中止本次分发并使 DecoderSink
失败，显式 Stop 仍会在解码线程退出后依次停止所有已注册消费者。

轨道就绪、时间线重置和结束是两条解码支路共同参与的边界，不与任何 Frame 回调
重叠。EOF 先排空缓存、解码器和音频重采样器，两条支路汇合后才通知下游一次结束，
随后 DecoderSink 为 `kEnded`。重连先到时会清除旧缓存和未消费的 Packet，刷新
解码及重采样状态后通知时间线重置；被直接替换的旧代次可以没有单独的结束通知。

新链路支持 Input → DecoderSink → Processor Sink 的组合。

下面的组合中，`frame_sink` 和 `another_frame_sink` 是调用方实现的消费 Frame 的 Sink，
配置由调用方提供：

```cpp
namespace decoding = mw::streamer::decoder;
namespace input = mw::streamer::input;
namespace pipeline = mw::streamer::pipeline;

auto decoder = std::make_unique<decoding::DecoderSink>("decoder", decoder_config);
decoder->AddSink(std::move(frame_sink));
decoder->AddSink(std::move(another_frame_sink));
// 借用指针用于查询状态；所有权仍交给 Pipeline。
auto* decoder_status = decoder.get();
pipeline::Pipeline flow(std::make_unique<input::ZlmInput>(input_config));
flow.AddSink(std::move(decoder));
flow.Start();
```

使用时另包含 `mw/decoder/decoder_sink.h`。

消费 Frame 的 Sink 回调抛出的异常会使 DecoderSink 进入 `kFailed`，原因可通过 `error()`
查询。显式 Stop 丢弃待处理数据、取消边界等待，等待解码线程退出后再停止下游。
需要完整处理有限输入时，应等 DecoderSink 达到 `kEnded` 后再调用 Pipeline.Stop；
Input 的 EOF 状态仅表示源读完。

### Fatal 错误与整链路停止

同步 Frame 处理节点的处理或边界回调可以抛出 `sink::FatalError`，
定义位于 `mw/sink/fatal_error.h`。
DecoderSink 捕获后先记录自身失败，再通过 Sink 的 fatal 通道请求 Pipeline
停机，同时丢弃本地排队数据并取消解码边界等待。独立的异步 Sink 可以调用
受保护的 `ReportFatalError()` 上报；Pipeline 注册 Sink 时绑定 `SetOnFatalError()`。
该通道只传递首次 fatal 的诊断信息，不能在报错线程中调用 Stop 或 Join。

Pipeline 收到请求后立即进入 `PipelineState::kFailed` 并停止继续分发输入通知，
控制线程依次停止 Input 和所有已注册的根 Sink，后者负责等待自身工作并停止下游。
已经执行中的回调允许完成，停机等待它们退出；并发 fatal 和外部 Stop 不会重复
停止组件。该机制停止整条 Pipeline，不终止宿主进程。

`Pipeline::error()` 保留首次 fatal 原因，正常停止状态和后续错误都不会覆盖它。
`state() == kFailed` 表示已发生错误，不表示停机已完成；外部调用 `Stop()` 可以
等待所有组件和控制线程退出，返回后状态仍为 `kFailed`。`input_status()` 单独
反映输入连接状态。普通 Frame 回调异常继续使用 DecoderSink 本地失败语义。
完整链路由 Pipeline 接管时才会自动全链路停机；单独使用 DecoderSink 时，调用方
可在开始投递前绑定 fatal 通知，并在自己的控制线程中完成停止。

### Processor Sink

`AnalysisProcessorSink` 和 `TransformProcessorSink` 都直接继承 `Sink` 并消费 Frame，按回调
是否产生输出区分，均可接在文件或实时输入的 DecoderSink 后面。

- `AnalysisProcessorSink` 使用 `FileProcessorConfig` 和
  `MwStreamerFileProcessorCallbacks`。回调只借用输入帧；没有输出缓冲区和下游。
  未注册某轨道回调时忽略该轨道。
- `TransformProcessorSink` 使用 `StreamingProcessorConfig` 和
  `MwStreamerStreamingProcessorCallbacks`，通过 `AddSink()` 独占持有下游，
  输入前至少注册一个消费者。`on_start` 可设置视频输出尺寸，未设置时为
  1920×1080；随后框架分配可写输出缓冲区。回调返回时必须完成输出，再按注册顺序
  同步交给下游。音频输出与输入保持相同的声道数和
  样本数，视频输出使用配置宽高；输出继承输入时间戳。

Transform 未注册音频或视频回调时，直接透传对应原帧引用，保留原始存储和元数据。
视频每帧的实际宽高必须等于配置的输出宽高，不符则抛出包含实际和期望尺寸的
`FatalError`，通过上述通道自动停止整个 Pipeline；不会自动缩放、补黑帧或丢弃
错误帧后继续。

两个 Sink 不增加媒体线程或媒体队列。音视频回调可以并发，同一轨道保持有序。
首次轨道就绪时启动业务，时间线重置通知业务清理时序状态，重连不会重复启动。
EOF 触发业务结束边界；Stop 等待在途回调并且只对成功启动的业务调用一次停止回调。
`UpdateConfig()` 更新业务配置字符串，可与媒体处理并发，Stop 会等待更新完成。
所有视图仅在回调期间有效；控制方法不能从本 Sink 或下游的回调中重入。

### Sink 消息通信

Pipeline 持有一个从池中提取的独占 Poller 和 `ID → Sink*` 索引，直接复用 Poller
自带的线程安全任务队列。节点所有权仍保留在 unique_ptr 媒体树中。Sink 只持有
`std::function<void(const SinkMessage&)>`，自身没有消息队列和消息线程。

```cpp
// 所有节点构造时传入 ID；组装好媒体链路后，在 Start 前按 ID 绑定。
flow.SetMessageReceiver("encoder", "processor");
flow.Start();
// encoder 内部调用 SendMessage(message)，由绑定函数投递到 Poller 任务队列。
```

`SetMessageReceiver()` 每个发送者只绑定一个接收者，重复设置会替换。未配置时
不绑定；目标可以是上游，也可以是其他分支。Pipeline 在输入启动前检查消息路由
两端的 ID 均存在，然后通过 `SetMessageSender()` 将目标 ID 和投递实现注入 Sink。
`AddSink()` 只建立媒体连接，不自动绑定消息接收者。

`SendMessage()` 返回 void，不提供投递或处理确认。Pipeline 的投递函数在返回前
复制来源、类型、二进制负载和可选时间戳，再唤醒消息 Poller。未绑定、Pipeline
未启动或已停止时发送直接忽略；接收节点尚未就绪或已停止时也忽略。有效投递中的
空指针与非零负载长度组合会抛参数错误，内存分配失败可抛异常。

每条消息通过 `poller.async(task, false)` 异步投递，任务持有消息副本，按目标 ID
调用 `OnMessage()`。不额外维护消息队列或分批调度逻辑。所有节点的消息回调在
同一 Poller 上串行执行，慢回调会延迟其他节点的消息。
默认 `OnMessage()` 忽略消息，不逐级转发；自定义 Sink 在业务初始化完成后调用
`StartMessages()` 启用接收，该方法只设置就绪状态。

两种 Processor 的 C 回调均提供可选 `on_message`。消息可与音视频处理及配置更新
并发，与 Processor 生命周期边界互斥，不与媒体建立全局时序。普通回调异常记录
后继续处理；`FatalError` 沿接收者所属的媒体树请求 Pipeline 停机。

Pipeline Stop 先关闭消息投递，让待处理任务跳过业务回调，再通过 `poller.sync()`
等待已提交任务和在途消息回调结束；
随后停止输入和全部 Sink。析构时保留消息设施直到所有 Sink 销毁，保证注入函数
的借用有效。消息回调不能直接调用 Pipeline/Sink 的控制或析构方法。
独立使用 Sink 时可自行注入发送函数，组合方负责它所借用资源的生命周期；
具体 Sink 析构函数仍须在自身状态销毁前调用 Stop。

### SynchronizerSink：实时同步与备播

`SynchronizerSink` 继承 `Sink`，输入输出均为 Frame，通过 `AddSink(unique_ptr<Sink>)`
连接一个或多个消费者，可放在 Processor 与 EncoderSink 之间，也可以直接连接
DecoderSink。Sink 拥有队列和调度逻辑，一个独立线程执行调度并串行调用下游。
投递只保留只读帧引用，队列满时淘汰旧原始帧，不等待下游释放容量。

`frame_queue_capacity` 默认 128，分别限制入口与调度缓存中的每一路音视频；
两级缓存合计最多保留四倍该容量的排队帧，另有少量原型、备播和在途帧引用。
`queue_depth()` 查询排队深度。CPU/CUDA 缓冲区保持引用共享，修改输出时间戳时
使用独立帧头，不修改 Processor 交来的帧。

首次取得所有声明轨道的原型后建立时间映射，并按当前保留队头的较晚 PTS 对齐。
在此之前，缺少的输出格式无法生成备播，缓存仍然有界。视频使用
`FrameStreamsReady` 中上游视频轨道的帧率，PTS 按累计帧数计算；缺少有效帧率时
拒绝启动。音频按累计样本数计时，静音也占据真实的输出样本位置。
固定单调时钟映射决定哪些源帧仍可使用；`max_frame_lateness` 默认 100 ms，
音视频共用这段固定播放缓冲，给帧到达留出余量，到释放期限才决定选帧或补帧。
选帧始终按原槽位对应的源时间计算，输出 PTS 不偏移，等待也不逐帧累计。
该配置同时限定候选源帧的最大过期时间，超过的帧被丢弃；设为 0 时不留播放缓冲。
同一代次收到迟到帧不会重设时间映射，因此不会把积压画面重新当作直播播放。

没有可用音频时输出静音；视频短暂缺帧时重复上一帧，持续缺帧超过
`standby_timeout`（默认 500 ms）后输出备播图片。`standby_image_path` 为空时
复用内置 loading 图片，指定路径时使用该图片；备播转换为原型的 CPU/CUDA 格式。
备播切入及恢复真实视频时请求 I 帧。即使 Processor 没有新回调或输入尚未报告
断线，调度线程也能继续输出。调度线程自身明显迟到时重新安排播放期限，避免
突发补发大量历史帧，同时保留源时间映射以淘汰过期结果。

输入断线、Reset 和重连由同步层消化，下游一直使用首次建立的输出代次，不因
备播重建编码器。新输入代次重新映射源时间，输出帧数和音频样本时钟连续；
轨道集合、源参数和实际输出格式必须保持兼容。最终 EOF 按节奏处理保留的媒体，
排空轨道不再补帧，随后向下游发送一次 EOF；EOF 通知到达前可能已有合法重复帧。
缺少初始轨道原型而收到 EOF 会明确失败。显式 Stop 丢弃积压，唤醒并等待调度线程，
然后停止各下游并释放原型资源。

`state()` 区分运行、备播、排空、结束和失败，`error()` 保存异步故障。基础参数和
输入顺序错误同步抛出；调度线程及下游异常记录为失败，显式 `FatalError` 或下游
fatal 报告沿既有通道请求停止整个 Pipeline。消息可以显式绑定接收者后直接投递给 Processor。
这里的实时丢帧发生在处理支路，直接连接 Input 的原始 Packet 录像仍遵守 RemuxSink
契约；交给 Remux 即完成交付，不等待网络发送结果。

### EncoderSink：Frame 到 Packet

`EncoderSink` 直接继承 `Sink`，消费原始音视频帧并输出编码 Packet，
通过 `AddSink(std::unique_ptr<Sink>)` 注册多个独占的 Packet 消费者。
编码实现负责编码器和执行调度，推流、录像交给下游 RemuxSink。

输入沿用上述 Frame 音视频并发与边界契约；输出 Packet 串行投递，
各消费者按注册顺序收到同一通知。每代次先交付编码后的轨道参数和时间基，再交付
Packet；EOF 在编码器排空、全部延迟包交付之后通知下游。Stop 等待在途交付完成后
停止所有消费者。下游至少注册一个，且只允许在首次输入通知和 Stop 之前注册。

`EncoderSink` 复用已有 `AudioEncoder` / `VideoEncoder`，
持有一个 BlockingQueue 和一个编码 Worker。音视频输入可以并发投递，Worker
串行编码并把同一份只读 Packet 缓冲区分发给多个独占的 Packet 消费者，不持有推流
地址、录像器或 Poller。

```cpp
#include "mw/encoder/encoder_sink.h"
#include "mw/output/remux_sink.h"
#include "mw/synchronizer/synchronizer_sink.h"

namespace encoding = mw::streamer::encoder;
namespace output = mw::streamer::output;
namespace synchronizer = mw::streamer::synchronizer;

encoding::EncoderSinkConfig encoder_config;
encoder_config.video_encoder.frame_rate = {25, 1};
auto encoder = std::make_unique<encoding::EncoderSink>("encoder", encoder_config);

output::RemuxSinkConfig recording;
recording.target = "/recordings/processed.mp4";
encoder->AddSink(
    std::make_unique<output::RemuxSink>("recording", std::move(recording)));
auto sync = std::make_unique<synchronizer::SynchronizerSink>("synchronizer");
sync->AddSink(std::move(encoder));
// processor 是已有的 TransformProcessorSink，可继续添加其他 Frame 消费者。
processor.AddSink(std::move(sync));
```

声明的轨道最多一路音频、一路视频。编码参数来自各轨道实际收到的首帧；音频
沿用 48 kHz float32 交错格式，视频支持现有 CPU/CUDA 编码路径，不隐式转换。
所有声明轨道的编码器打开后，先交付编码后的 StreamsReady，再交付缓存的启动
Packet。`frame_queue_capacity` 和 `startup_packet_capacity` 默认均为 256，分别
限制待编码帧和等待轨道就绪的编码包；生命周期通知不占帧配额。

时间线重置丢弃旧代次排队帧及编码器延迟，重建下一代次编码器，并在新的轨道信息
之前通知下游 Reset。暂时中断不排空编码器、不伪造 EOF；真正 EOF 则编码剩余
音频 FIFO 样本、排空两轨延迟包后交付下游 EOF。声明了轨道但从未收到首帧时，
EOF 会使 EncoderSink 失败。显式 Stop 关闭入口、丢弃积压、等待编码 Worker 退出，
最后停止各消费者。

`state()` / `error()` 提供线程安全快照。提交/编码错误只使 EncoderSink 失败；
已就绪下游收到失败结束通知。下游自身的普通故障由该 Sink 处理，显式 fatal
沿 Sink 故障通道请求 Pipeline 停机。同步、补帧、输出节奏和备播不属于本层。
接入 RemuxSink 录像时，编码延迟导致提前到达的音频和 EOF 排出的尾包都会被保留，
不再按首视频关键帧的到达顺序裁剪音频。

### 性能快照

`Pipeline::GetPerformance()` 返回拥有全部数据的统计树，包含 Input、直属 Sink
及其下游。Pipeline 只汇总，按类型查询和窗口速度计算都在快照上完成，不增加统计
线程，也不因读取而清空计数。Sink 节点 ID 使用构造时传入的 ID，Input 使用
`input`；多个相同类型的 Sink 通过各自的 ID 区分，同一节点的处理项使用固定枚举：

`kInput`、`kAudioDecoder`、`kVideoDecoder`、`kAudioProcessor`、
`kVideoProcessor`、`kSynchronizer`、`kAudioEncoder`、`kVideoEncoder`、`kRemux`。

```cpp
auto previous = pipeline.GetPerformance();
// 在上层下一次定时采样时执行；每个读取者按顺序保留自己的上一份快照。
auto current = pipeline.GetPerformance();
auto snapshot = current.WithRatesSince(previous);
for (const auto& match : snapshot.Find(
         mw::streamer::performance::PerformanceType::kVideoEncoder)) {
  fmt::print("{}: input {:.2f} frame/s, output {:.2f} packet/s\n",
             match.node->id, match.operation->input_per_second,
             match.operation->output_per_second);
}
previous = std::move(current);
```

原始快照的计数从对象创建开始累计，重连、时间线重置和 Stop 不清零。
`WithRatesSince()` 返回带窗口速率的副本，累计值保持不变；第一份原始快照没有
窗口速率，`rates_available` 为 false。查询结果借用当前快照，不能在快照移动、
修改或销毁后使用；快照数据本身可在 Pipeline 销毁后继续使用。不同读取者无需
共享采样基线，但不能把并发遍历的两份快照当作严格先后关系。

统计口径：

- 解码输入按真正开始处理的 Packet 计数；音频输出按重采样后的每通道采样数，
  视频输出按帧数。编码输入分别按采样数、帧数，输出按编码 Packet 数。
- 实际处理调用与入队分开，Decode/Encode 的 Drain 调用也计入耗时和调用数，
  不增加输入媒体数量。`completed_calls` 包含失败调用，`failed_calls` 是其子集；
  `in_flight` 能反映尚未返回的调用。
- 耗时为主机侧自身处理时间，排除同步下游执行；不插入 GPU 同步。累计 HDR
  分位数位于 `lifetime_latency`，窗口平均耗时位于 `interval_mean_time`，
  不将两次 P95 相减冒充窗口 P95。
- Processor 透传计输入输出量，但没有业务回调就不产生回调调用数。Analysis
  没有输出。原始 Frame 不猜测 CPU/GPU 存储字节数，相关 bytes 字段保持零。
- Synchronizer 的 `kSynchronizer` 以音视频 Frame 对象总数计量，输出包含重复、
  备播和静音帧；不能将它当作真实视频 FPS。调用耗时覆盖调度器 Push/TakeReady，
  不含等待播放时刻和下游处理。
- Remux 输入表示实际取出处理的 Packet，输出表示已交给本地 muxer 的 Packet。
  启动缓存中的包在真正写出（包括 EOF 排空）时计输出；不表示远端收到了数据。
- 音视频统计项固定保留；零值表示尚未处理，不代表声明了该媒体轨道。自定义
  Input 可覆盖 `GetPerformance()`，Sink 覆盖 `GetOwnPerformance()`，并使用
  `OperationRecorder` 接入；Sink 基类统一递归收集子节点。查询可与处理及 Stop 并发，不可与注册下游或析构并发。

Pipeline 与 e2e 均使用统一的 Input、Sink 链路；配置支持双向转换和结构体构建。

## 现有 PlayerProxy 输入接口

`mw/sink/packet_sink.h` 定义压缩音视频消费者 `mw::streamer::sink::PacketSink`。
单源输入 `input::PlayerProxy` 可在首次 `Start()` 前通过 `AddPacketSink()` 接收
`std::unique_ptr<PacketSink>`，取得 Sink 的独占所有权。多个 Sink 按注册顺序在
输入所属 poller 上同步调用；Sink 自己决定是否排队、使用工作线程及如何处理积压和
错误，方法不得向输入层抛出异常。阻塞投递会同时阻塞输入和后续 Sink。

每个输入代次先收到 `SetStreams()`，再收到 `Write()`。轨道信息和 Packet 仅在调用
期间借用；异步使用时，Sink 需复制轨道信息、为 Packet 持有引用，并保持共享媒体
数据只读。Seek 通过新的 `SetStreams()` 替换旧时间线；断流、EOF 和主动停止通过
`EndInput()` 结束已打开的当前代次，EOF 通知发生在尾包交付之后。`EndInput()` 不等于
销毁或等待 Sink 排空。输入停止后仍保留已注册的 Sink，供再次启动时使用；
`PlayerProxy` 析构时在 poller 上结束当前代次并销毁 Sink。

`ZlmInput` 在此接口上适配通用 Input 事件。`SetOnState()` 保留连接、重试和失败
通知，`SetOnTimelineReset()` 保留底层 Seek 的位置与代次反馈；这些接口属于
PlayerProxy，通用 Input 当前只公开 Start、Stop 与状态查询。

## Pipeline 配置与 TOML 双向转换

链路使用 `pipeline::PipelineConfig`，不按实时、文件或转封装划分配置类型。
`mw/pipeline/pipeline_config.h` 定义一个 Input 和平铺的节点列表；每个节点保留
`id`、`downstream`、可选的 `message_receiver`，并通过具体 NodeConfig 的
`options` 成员复用已有参数结构。配置对象独占持有节点描述，只能移动，不包含
运行中的 Sink、线程或业务回调。

完整模板见 [`template/pipeline.toml`](template/pipeline.toml)。它使用 `[input]`
和 `[[sinks]]` 描述原始录像及解码、处理、同步、编码后的多目标输出。节点声明
可以前向引用；媒体投递顺序取决于 `downstream` 数组，与声明顺序无关。
可用节点类型为 `decoder`、`analysis_processor`、`transform_processor`、
`synchronizer`、`encoder` 和 `remux`。Input 支持两种类型：

- `type = "zlm"` 使用 `url`，接受 ZlmInput 支持的网络地址和实时文件播放。
- `type = "file"` 使用 `path`，通过 FileInput 全速读取本地文件；不配置 player 或重连参数。

文件分析仍使用同一份 Pipeline TOML 格式：

```toml
[input]
type = "file"
path = "./input.mp4"
downstream = ["decoder"]

[[sinks]]
id = "decoder"
type = "decoder"
downstream = ["analysis"]
[sinks.video_decoder]
backend = "software"

[[sinks]]
id = "analysis"
type = "analysis_processor"
[sinks.config]
mode = "offline"
```

结构体构建时，设置 `spec.input.type = pipeline::InputType::kFile` 和
`spec.input.file.path = "./input.mp4"`，其余节点构建方式相同。

`mw/config/toml.h` 提供四个统一入口：

- `ParsePipelineConfigFromToml(text)`：TOML 字符串转结构体。
- `SerializePipelineConfigToToml(config)`：结构体转 TOML 字符串。
- `LoadPipelineConfigFromToml(path)`：从文件加载。
- `SavePipelineConfigToToml(config, path)`：保存到文件，写入前先序列化并校验。

双向转换保留节点参数、声明及下游顺序、消息接收者、编码属性和嵌套业务配置的
语义，不保留注释、空白、引号样式或字段排版。序列化显式写出默认参数。Processor
的 `options.config` 在 TOML 序列化时必须是有效 TOML 文本（空字符串表示空表），
写入对应的 `[sinks.config]`；库不解释业务字段，不将无效文本静默转换为字符串。

字符串解析保留路径原文；文件加载把本地输入、Remux 录像目标和备播图片的相对
路径解析为相对于 TOML 所在目录的绝对路径，URL 与可选空路径保持不变。加载后
再保存会写出已经解析的路径，因此另存到其他目录不会改变媒体文件位置。
手工结构体中的相对路径交给运行组件时以调用方工作目录为基准；保存相对路径后，
文件加载使用上述 TOML 目录规则。

解析、序列化和构建都校验空/重复 ID、未知引用、媒体类型不匹配、多个媒体上游、
不可达节点、环路和可静态检查的参数。未知 TOML 字段会被忽略并记录警告，错误类型和整数越界仍直接报错。
每个 Sink 只有一个媒体上游，消息连接独立；实际轨道、尺寸兼容与编解码器可用性
仍由运行组件检查。

`mw/pipeline/pipeline_builder.h` 的 `BuildPipeline()` 同时服务 TOML 与 C++ 调用方：

```cpp
namespace pipeline = mw::streamer::pipeline;
namespace config = mw::streamer::config;

pipeline::PipelineConfig spec;
spec.input.options.url = "rtsp://127.0.0.1/live/camera";
spec.input.downstream = {"recording"};
auto recording = std::make_unique<pipeline::RemuxNodeConfig>("recording");
recording->options.target = "./original.mp4";
spec.sinks.push_back(std::move(recording));

// 可以直接构建，也可以先保存、传输或编辑 TOML。
auto flow = pipeline::BuildPipeline(spec);
auto text = config::SerializePipelineConfigToToml(spec);
auto restored = config::ParsePipelineConfigFromToml(text);
config::SavePipelineConfigToToml(restored, "pipeline.toml");
```

构建器复制节点参数并组装 unique_ptr 媒体树及消息路由，不启动输入，也不借用
配置对象。宿主通过可选 `ProcessorBindings` 的 `analysis` / `transform` 字典，
按 Sink ID 提供对应的纯 C 回调；这些绑定不会写入 TOML。缺省回调仍遵守现有的
忽略/透传语义，回调的 user_context 由宿主保留到 Pipeline 停止。原有手工
`Pipeline::AddSink()` 组装方式继续可用。

## 初始化配置

进程初始化使用 `LoadInitConfigFromToml()`，模板见
[`template/init.toml`](template/init.toml)，包含日志和 ZLToolKit 线程配置。
链路只使用 [`template/pipeline.toml`](template/pipeline.toml) 的统一格式。
未填写的可选字段沿用 C++ 默认值；未知字段会被忽略并记录警告，错误类型及整数越界会直接报错。

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
