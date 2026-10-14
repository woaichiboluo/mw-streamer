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
