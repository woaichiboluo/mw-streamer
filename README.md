# mw-streamer

面向 Windows 和 Linux 的 C++17 音视频处理库，目前正在开发中。

提供媒体输入、解码、滤镜处理、音视频同步、编码、封装、推流、本地发布和文件录制能力。

使用 CMake 构建，支持静态库和动态库。主要依赖 FFmpeg、ZLMediaKit、OpenSSL 和 SRT。

## 输入帧率

`AvProbe` 和 `AvInput` 的 `StreamInfo.frame_rate` 保存输入的平均帧率
（`AVStream::avg_frame_rate`），与 FFmpeg 显示的 `fps` 一致。
`base_frame_rate` 单独保存 `r_frame_rate`（`tbr`），`decoder_frame_rate`
保存解码器使用的 `av_guess_frame_rate()` 提示。平均帧率未知时保持未知，
自动输出配置不得用基础帧率冒充平均帧率；显式配置的输出帧率独立生效。
可变帧率输入仍按源 PTS 调度，平均 FPS 不表示每帧具有相同持续时间。

新增字段改变了 `StreamInfo` 的 ABI；升级时须一起重新编译 SDK 和所有调用方。

## 编码包交付与录制

`RemuxPipeline` 默认立即交付输入编码包，不按播放时钟等待。实时流录制不再额外
节流；本地文件或网络点播文件的转封装按可处理的速度完成，媒体时间戳和播放时长
保持不变。

只有需要将文件按播放速度推送时，才显式设置 `RemuxPipelineConfig::pace = true`。
此时 `speed` 和 `max_ahead` 控制交付节奏，`SetSpeed()` 可动态调整倍速。输入 URL
的协议不用于推断直播或点播。落后于其他轨道的旧包不能拉回共享调度时钟。

默认交付行为及 `RemuxPipelineConfig` 的 ABI 已改变；升级时一起重编 SDK 和调用方。
原来依赖默认限速的文件推流调用须显式开启 `pace`。

## 性能日志

直播优先开启 `InitConfig.performance.stages[PerformanceStage::kLivePipeline]`
的 `PerformanceMode::kSummary`，每约两秒输出一条 `streamer.perf`。
先看 `output` 和 `process.headroom`，都先列最近一次报告，再列最近最多五次报告。
`output` 显示滤镜实交帧数和参考帧数。参考帧数由编码目标 FPS 乘实际经过时间得出，
源帧率与编码目标不同时，优先按 `input` 和 `encoder` 判断两侧流量。
`headroom` 是目标帧间隔减去平均滤镜耗时，单位为毫秒；负值表示平均耗时超过该预算。
`input.read`、`input.processed`、编码送入、出包和重同步只统计最近一次报告。
`encoder.submitted` 含帧率转换和补帧，`encoder.packets` 按视频包计数。
重同步从第二次建立映射开始计数，编码阻塞按每次新阻塞计数。

排查单个阶段时，可在调用 `Init` 前将对应的 `InitConfig.performance.stages` 项设为
`PerformanceMode::kSummary`。例如 `PerformanceStage::kAudioResampler` 的记录
同样使用 `streamer.perf`，通过 `stage=filter.audio_resampler` 识别。

## Live 事件剪辑

`LivePipelineConfig.clip.enabled` 默认为 `false`。启动前开启并设置
`cache_duration`（默认 30 秒）和 `max_cache_bytes`（默认 64 MiB），即可在编码输出
旁维护按 GOP 对齐的历史缓存。`OfflinePipeline` 不接入此功能；运行中不动态启停。

业务在事件来源帧进入处理时保存 `pipeline.clip_generation()` 和该帧的
`pts/time_base`。异步检测或业务 FIFO 延迟后发布事件时，仍使用当时保存的代次和
来源时间，不能读取通知时刻的当前时间。使用 `ClipRequest` 指定 `event_id`、
`time`、`before`、`after` 和本地 MP4 `path`，再调用 `pipeline.SubmitClip(request)`。
路径父目录须存在；业务为不同事件选择不同路径，并自行避免事件重叠。

Pipeline 仅负责转交请求，未创建剪辑模块时记录 ERROR。剪辑模块负责历史时间映射、
编码包缓存、GOP 边界选择和异步录盘，所有剪辑相关错误、终止和完成均通过日志输出，
没有返回值或结果回调，失败不停止直播。剪辑不会重新编码或改变直播的音视频时间线。
开始和结束按可解码 GOP 对齐，实际文件可能包含请求窗口以外的画面。
输入代次改变会丢弃旧缓存、旧映射和未完成剪辑；已经写出的部分文件保留并记录终止。

新增配置和接口会改变 C++ ABI，SDK 和调用方须一起重新编译。
