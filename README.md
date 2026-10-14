# mw-streamer

面向 Windows 和 Linux 的 C++ 音视频处理库，目前正在开发中。

## AvEncoder

独立编码组件使用 `AvEncoderConfig`，构造时传入共享的
`ffmpeg::HwDeviceContext`，CPU 编码同样传入 CPU 上下文。
`Start(config)` 同步打开启用的编码器，成功返回 `true`，随后可以通过 `streams()`
查询输出轨道；失败记录日志并返回 `false`，异常保存在 `error()`，清理后允许重试。
编码结果通过 `SetOnPacket()` 交付，线程内错误通过 `SetOnError()` 交付
`std::exception_ptr`。本组件不负责封装或推流。

视频默认 NV12，音频默认 FLTP；对应格式为 NONE 时禁用该轨道，不能全部禁用。
视频尺寸必须明确。码率、GOP、B 帧和码率模式使用配置中的正式字段，编码器私有
参数通过 `video_options`、`audio_options` 指定，不能覆盖正式字段。

内部拥有一个帧线程，并用 TBB 并行处理音视频；实时队列缓存一帧，满时丢弃新帧，
时间计数仍然推进。`Stop()` 或析构会拒绝新帧，按队列顺序排空音频滤镜和编码器。
packet 回调可能并发执行；不能在回调中停止或销毁编码器，回调依赖的对象需要存活
到 `Stop()` 返回。

构建需要可被 CMake 找到的 oneTBB 开发包和运行库，例如 Debian 的 `libtbb-dev`；
Windows 或自定义工具链可以通过 `CMAKE_PREFIX_PATH` 指定其安装目录。
TBB 是 CPU 任务调度依赖，不是硬件编解码接口。

## EncodePushSink

`EncodePushSink` 实现 `FrameSink`，构造时传入共享的 CPU/CUDA 设备，并用
`unique_ptr` 持有 Encoder 和 Remuxer。`Initialize(format)` 先启动 Encoder，
再用编码轨道启动 Remuxer；两者都成功才返回 `true`。失败会停止已启动的
Encoder，保留登记的输出目标供重试；成功初始化后不能重复初始化。

`Send(OutputFrame)` 返回已经完成的 promise future，表示 Encoder 接收或按实时
策略主动丢帧，不等待编码和网络发送。它复用 Encoder 的一帧队列，没有新增线程
或队列，也不提供同步时钟。编码包由内部 `OnPacket` 回调提交到 Remuxer。
同步参数错误进入 future；首次异步终止错误通过 `SetOnError()` 的异常指针回调
通知，之后 `Send` 返回 `false`，由 Output 停止向这个 sink 分发。

`AddPushUrl()`、`AddRtspPublish()`、`AddHlsPublish()` 和 `output_info()`
保留 Remuxer 的完整输出能力，支持初始化前登记和运行中动态添加。
销毁时先排空 Encoder，再关闭 Remuxer；错误回调里只能查询输出信息，不能
初始化、发送、添加输出或销毁 sink。捕获的回调资源应存活到 sink 销毁完成。

## 解码与组帧错误回调

`AvDecoder` 和 `AvFrameSource` 使用 `OnError(std::exception_ptr)`，构造后通过
`SetOnError()` 注册，再向 Decoder 投递压缩包。`AvFrameSource` 借用 Decoder
缓存进行滤镜处理和音视频组帧；`AvMediaInput` 在内部用相同接口接收两者的失败通知，
对外通过 `SetOnStateChanged()` 上报状态和异常，`kFailed` 表示终止失败，
`kWaitingRetry` 表示正在重试，异常保存在状态的 `error` 字段。
回调可以替换或清除，工作线程在锁外通知；已经发生的失败不重放，可通过
`error()` 查询原始异常。构造期间的初始化失败仍同步抛出。

## Remuxer

`Remuxer` 接收 H264/H265 视频和 AAC 音频编码包，支持多目标推流、本地
RTSP/HLS 发布和 MP4/HLS 文件录制。使用前调用 `Init`，将
`AvEncoder::streams()` 的编码轨道传给 `Start`；`Start` 成功返回 `true`，
失败记录日志、清理启动资源并返回 `false`，可以重试。成功后通过
`SubmitPacket` 投递编码包；网络连接结果仍然由 ZLM 异步处理。

唯一的对外回调是 `OnError(std::exception_ptr)`，只通知当前会话无法继续的
终止错误，每个会话通知一次，通知前拒绝新包并安排关闭。处理异常保留原始类型，
ZLM 不可恢复的输出错误在输出层包装为包含目标、错误码和描述的
`std::runtime_error`。可恢复的网络失败只记录日志，由 ZLM 自动重连，不触发回调。
启动失败不触发错误回调，添加输出的同步失败直接抛异常。错误回调不能调用 `Start`、`Stop`
或添加输出；回调自身抛出的异常会记录日志。

`output_info()` 返回 `Remuxer::OutputInfo`，包含推流/录制地址及 RTSP、HLS
发布地址。`Drain` 拒绝新包并安排排空，`Stop` 等待输出关闭完成；对象销毁后
再调用 `Shutdown`。停止后重新启动需要重新登记输出目标。

正式构建包含仓库内的 ZLMediaKit，并要求可被 CMake 找到的 OpenSSL、SRT
和 oneTBB 开发包，以及 FFmpeg 的 avdevice 组件。Remuxer 测试涵盖时间戳、
交织、包转换、真实录制解码、错误回调及生命周期。
