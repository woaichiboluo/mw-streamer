# mw-streamer

面向 Windows 和 Linux 的 C++17 音视频处理库，目前正在开发中。

提供媒体输入、解码、滤镜处理、音视频同步、编码、封装、推流、本地发布和文件录制能力。

使用 CMake 构建，支持静态库和动态库。主要依赖 FFmpeg、ZLMediaKit、OpenSSL 和 SRT。

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
