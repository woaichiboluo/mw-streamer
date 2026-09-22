# mw-streamer 配置参考

本文档列出统一 Pipeline TOML 支持的全部配置字段，以及每种 Sink 的媒体契约、
默认值和静态校验规则。配套的说明性配置字典见
[`template/configuration_reference.toml`](../template/configuration_reference.toml)。

说明性 TOML 用于查阅和复制字段，不表示一条可运行拓扑。实际 Pipeline 配置使用
`[input]` 和一个或多个 `[[sinks]]`，并通过 `BuildPipelineFromToml()` 加载。
日志和 ZLToolKit 线程池属于进程运行时，分别通过 `MwLogConfig`、`MwZlmConfig`
传给 `mw_streamer_initialize()`。

## 通用约定

- 标记为“必填”的字段必须出现在 TOML 中；其他字段省略后使用表中的默认值。
- 所有以 `_ms` 结尾的字段单位均为毫秒，名称与公开 C++ 配置字段一致。
- TOML 整数是有符号 64 位整数；映射到 C++ 容量字段时不能为负数。
- 从文件加载 TOML 时，本地输入、录像目标和备播图片的相对路径以 TOML 所在目录
  为基准。直接构造 C++ 配置时，相对路径以调用方工作目录为基准。
- 当前解析器会记录并忽略未知字段；类型错误、未知枚举、整数越界和静态配置错误
  会抛出异常。
- Pipeline TOML 中出现旧的根级 `[log]` 或 `[zlm]` 会报错，避免运行时配置被静默忽略。

## 进程运行时配置

调用 `mw_streamer_initialize(&log_config, &zlm_config)` 前，分别使用
`mw_log_default_config()` 和 `mw_zlm_default_config()` 填入默认值，再覆盖需要调整的
字段。初始化会同步复制所有字符串，不保留调用方内存。

`MwLogConfig::modules` 使用分号分隔模块。裸模块名使用 `info`，也可以写成
`ffmpeg:trace;streamer:warn;`。支持 `trace`、`debug`、`info`、`warn`、`error`、
`critical`、`off`。未配置的 `default` 模块默认为 `info`，其他模块默认为 `error`。

| `MwLogConfig` 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `modules` | `streamer;processor;` | 模块级别覆盖字符串。 |
| `console_enabled` | `1` | 是否创建 Console Sink。 |
| `console_color` | `1` | Console 是否使用颜色。 |
| `rotating_file_enabled` | `0` | 是否创建滚动文件 Sink。 |
| `rotating_file_path` | 空 | 文件启用时必须非空。 |
| `rotating_file_max_size` | `10485760` | 单文件最大字节数。 |
| `rotating_file_max_files` | `5` | 保留文件数。 |
| `async_enabled` | `0` | 是否异步输出。 |
| `async_queue_size` | `8192` | 异步队列容量。 |
| `async_overflow` | `overrun_oldest` | 也可选择 `block`。 |

Console 与滚动文件不再各自配置级别，均接收模块过滤后的全部日志。

| `MwZlmConfig` 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `event_poller_threads` | `0` | 0 表示按硬件并发数决定。 |
| `work_threads` | `0` | 0 表示按硬件并发数决定。 |
| `enable_cpu_affinity` | `1` | 同时作用于两个线程池。 |

## Input

每条 Pipeline 只有一个 `[input]`。Input 固定输出压缩 Packet，因此
`input.downstream` 引用的首层 Sink 必须接收 Packet。

### 公共字段

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 必填 | 约束与说明 |
| --- | --- | --- | --- | --- | --- |
| `input.type` | `InputConfig::type` | string enum | 无 | 是 | 可取 `zlm`、`file`。 |
| `input.downstream` | `InputConfig::downstream` | string array | 无 | 是 | 至少包含一个存在的 Sink ID。 |

### `type = "file"`

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 必填 | 约束与说明 |
| --- | --- | --- | --- | --- | --- |
| `input.path` | `FileInputConfig::path` | string | 无 | 是 | 非空本地媒体文件路径；全速离线读取。 |

File Input 不接受 `url`、`player` 或 `reconnect_policy`。其 Decoder Sink 的
`cache_duration_ms` 必须为 0。

### `type = "zlm"`

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 必填 | 约束与说明 |
| --- | --- | --- | --- | --- | --- |
| `input.url` | `ZlmInputConfig::url` | string | 无 | 是 | 非空，交给 ZLMediaKit 打开。 |
| `input.player.connect_timeout_ms` | `PlayerConfig::connect_timeout_ms` | integer | `10000` | 否 | 1～`INT_MAX`。 |
| `input.player.media_timeout_ms` | `PlayerConfig::media_timeout_ms` | integer | `5000` | 否 | 1～`INT_MAX`。 |
| `input.player.local_bind_ip` | `PlayerConfig::local_bind_ip` | string | `""` | 否 | 拉流连接使用的本地绑定 IP；空表示不指定。 |
| `input.reconnect_policy.max_retries` | `ReconnectPolicy::max_retries` | integer | `-1` | 否 | 初次连接后的重试次数；-1 表示无限重试，其他值必须非负。 |
| `input.reconnect_policy.min_delay_ms` | `ReconnectPolicy::min_delay_ms` | integer | `2000` | 否 | 必须大于 0。 |
| `input.reconnect_policy.max_delay_ms` | `ReconnectPolicy::max_delay_ms` | integer | `60000` | 否 | 不得小于 `min_delay_ms`。 |
| `input.reconnect_policy.delay_step_ms` | `ReconnectPolicy::delay_step_ms` | integer | `3000` | 否 | 必须大于 0。 |

## Sink 总览

| `type` | 输入媒体 | 输出媒体 | 是否终端 | 专属 TOML 配置 |
| --- | --- | --- | --- | --- |
| `decoder` | Packet | Frame | 否 | 有 |
| `analysis_processor` | Frame | 无 | 是 | 无 |
| `transform_processor` | Frame | Frame | 否 | 无 |
| `frame_custom` | Frame | 无 | 是 | 无 |
| `packet_custom` | Packet | 无 | 是 | 无 |
| `synchronizer` | Frame | Frame | 否 | 有 |
| `encoder` | Frame | Packet | 否 | 有 |
| `remux` | Packet | 无 | 是 | 有 |

“无”表示该节点不再向下游输出媒体。消息通过 `Pipeline::SubmitMessage()` 按目标
Sink ID 投递，不需要在配置中声明连接。

### Sink 公共字段

以下字段直接写在每个 `[[sinks]]` 中。

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 必填 | 约束与说明 |
| --- | --- | --- | --- | --- | --- |
| `id` | `SinkConfig::id` | string | 无 | 是 | 非空且在整个 Pipeline 内唯一。 |
| `type` | 具体 `NodeConfig` 类型 | string enum | 无 | 是 | 必须是上表列出的八种类型之一。 |
| `downstream` | `SinkConfig::downstream` | string array | `[]` | 否 | Encoder 可为空；其他非终端节点至少一个；终端节点必须为空。 |

## Decoder Sink

`type = "decoder"`，媒体契约为 Packet → Frame，必须配置媒体下游。

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 约束与说明 |
| --- | --- | --- | --- | --- |
| `cache_duration_ms` | `DecoderSinkConfig::cache_duration_ms` | integer | `0` | 只能为 0 或 1000～30000；File Input 下必须为 0。 |
| `audio_decode_queue_capacity` | `DecoderSinkConfig::audio_decode_queue_capacity` | integer | `256` | 必须大于 0；每音轨等待解码的 Packet 容量。 |
| `video_decode_queue_capacity` | `DecoderSinkConfig::video_decode_queue_capacity` | integer | `128` | 必须大于 0；每视频轨等待解码的 Packet 容量。 |
| `audio_decoder.decoder_name` | `AudioDecoderConfig::decoder_name` | string | `""` | 空表示由 FFmpeg 按输入 codec 选择默认解码器。 |
| `video_decoder.decoder_name` | `VideoDecoderConfig::decoder_name` | string | `""` | 空表示由 FFmpeg 按输入 codec 选择默认解码器。 |
| `video_decoder.backend` | `VideoDecoderConfig::backend` | string enum | `cuda` | 可取 `software`、`cuda`。 |
| `video_decoder.device_index` | `VideoDecoderConfig::device_index` | integer | `0` | CUDA 后端下必须非负。 |

队列容量不包含上游时间缓存和生命周期消息。音频和视频分别使用独立解码队列。

## Analysis Processor Sink

`type = "analysis_processor"`，媒体契约为 Frame → 无，是终端 Sink，不能配置
`downstream`，没有专属 TOML 字段。

业务回调通过 `ProcessorBindings::analysis` 按 Sink ID 注入，可以省略。Processor
业务配置不属于 TOML，由 Processor 自身维护。

## Transform Processor Sink

`type = "transform_processor"`，媒体契约为 Frame → Frame，必须配置媒体下游，
没有专属 TOML 字段。

业务回调通过 `ProcessorBindings::transform` 按 Sink ID 注入，可以省略。未提供某类
处理回调时使用既有透传语义。Processor 业务配置由其自身维护。

## Frame Custom Sink 和 Packet Custom Sink

`type = "frame_custom"` 的媒体契约为 Frame → 无；`type = "packet_custom"`
的媒体契约为 Packet → 无。两者都是终端 Sink，不能配置 `downstream`，没有专属
TOML 字段。旧的 `custom` 类型更名为 `frame_custom`。

每个节点必须按 ID 提供对应类型的回调绑定，否则构建失败：

- Frame：`ProcessorBindings::frame_custom_sinks`，使用
  `MwStreamerFrameCustomSinkCallbacks`。
- Packet：`ProcessorBindings::packet_custom_sinks`，使用
  `MwStreamerPacketCustomSinkCallbacks`。

回调绑定不写入 TOML。两者提供可选的 `on_start`、`on_stop`、`on_message` 和
`on_boundary`。`on_start` 返回启动结果，成功启动后实际停止时调用一次 `on_stop`。
EOF 只通知 `on_boundary`，不会自动停止。媒体回调同步执行，不增加缓存或线程。

Frame 使用 `on_frame`、`on_audio`；Packet 使用 `on_video_packet`、
`on_audio_packet`。Packet 回调的 `const void*` 实际指向 `const AVPacket`，
仅在回调期间有效，不得修改或释放。业务需要异步使用时应自行在回调中取得引用，
例如调用 `av_packet_clone()`，并在使用结束后释放；公共回调头文件不依赖 FFmpeg。

## Synchronizer Sink

`type = "synchronizer"`，媒体契约为 Frame → Frame，必须配置媒体下游。

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 约束与说明 |
| --- | --- | --- | --- | --- |
| `frame_queue_capacity` | `SynchronizerSinkConfig::frame_queue_capacity` | integer | `128` | 必须大于 0；每轨输入队列和调度队列分别使用该容量。 |
| `max_frame_lateness_ms` | `SynchronizerSinkConfig::max_frame_lateness_ms` | integer | `100` | 必须非负；公共音视频播放缓冲，增加固定墙钟延迟。 |
| `standby_timeout_ms` | `SynchronizerSinkConfig::standby_timeout_ms` | integer | `500` | 必须非负；无可用视频帧后切换备播图的等待时间。 |
| `standby_image_path` | `SynchronizerSinkConfig::standby_image_path` | string | `""` | 空表示使用内置 loading 图。 |

容量饱和时会丢弃较旧的原始帧，生产者不等待容量。缺失音频时补静音；视频短时
缺失时重复上一帧，超过 `standby_timeout_ms` 后使用备播图。

## Encoder Sink

`type = "encoder"`，媒体契约为 Frame → Packet，媒体下游可以为空。
没有下游时仍正常编码和统计，编码包立即释放，不等待消费者或保留启动包缓存。

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 约束与说明 |
| --- | --- | --- | --- | --- |
| `frame_queue_capacity` | `EncoderSinkConfig::frame_queue_capacity` | integer | `256` | 必须大于 0；生命周期通知不占配额。 |
| `startup_packet_capacity` | `EncoderSinkConfig::startup_packet_capacity` | integer | `256` | 必须大于 0；全部声明轨道打开编码器前的 Packet 缓存容量。 |
| `audio_encoder.encoder_name` | `AudioEncoderConfig::encoder_name` | string | `""` | 空表示选择 FFmpeg 默认 AAC 编码器。 |
| `audio_encoder.properties.<key>` | `AudioEncoderConfig::properties` | string map | `{}` | 任意 FFmpeg 编码器属性；键和值都必须是字符串。 |
| `video_encoder.codec` | `VideoEncoderConfig::codec` | string enum | `h264` | 可取 `h264`、`h265`。 |
| `video_encoder.encoder_name` | `VideoEncoderConfig::encoder_name` | string | `""` | 空时 CUDA 帧选择 NVENC，Host 帧选择 FFmpeg 默认编码器。 |
| `video_encoder.frame_rate.num` | `VideoEncoderConfig::frame_rate.num` | integer | `0` | 必须非负。 |
| `video_encoder.frame_rate.den` | `VideoEncoderConfig::frame_rate.den` | integer | `1` | 必须大于 0。 |
| `video_encoder.properties.<key>` | `VideoEncoderConfig::properties` | string map | `{}` | 任意 FFmpeg 编码器属性；键和值都必须是字符串。 |

Encoder 不负责音视频同步、节奏控制、备播、像素格式转换或内存类型转换。输入格式
必须能被所选编码器直接消费。

## Remux Sink

`type = "remux"`，媒体契约为 Packet → 无，是终端 Sink，不能配置
`downstream`。每个 Remux Sink 只配置一个输出目标，不进行编解码。

| TOML 字段 | C++ 字段 | 类型 | 默认值 | 必填 | 约束与说明 |
| --- | --- | --- | --- | --- | --- |
| `target` | `RemuxSinkConfig::target` | string | 无 | 是 | `rtmp://`、`rtsp://`、`srt://`，或以 `.mp4`、`.m3u8` 结尾的文件路径。 |
| `packet_queue_capacity` | `RemuxSinkConfig::packet_queue_capacity` | integer | `384` | 否 | 必须大于 0；投递队列和启动缓存分别使用该容量。 |
| `zlm.pusher.connect_timeout_ms` | `PusherConfig::connect_timeout_ms` | integer | `10000` | 否 | 1～`INT_MAX`。 |
| `zlm.pusher.local_bind_ip` | `PusherConfig::local_bind_ip` | string | `""` | 否 | 推流连接使用的本地绑定 IP；空表示不指定。 |
| `zlm.muxer.paced_sender_interval_ms` | `MuxerConfig::paced_sender_interval_ms` | integer | `0` | 否 | 0～`UINT32_MAX`；0 禁用 ZLM paced sending。 |
| `zlm.recording.file_buffer_size` | `RecordingConfig::file_buffer_size` | integer | `65536` | 否 | 1～`UINT32_MAX` 字节。 |
| `zlm.recording.hls_segment_duration_ms` | `RecordingConfig::hls_segment_duration_ms` | integer | `10000` | 否 | 1～`UINT32_MAX`。 |

`.mp4` 输出为 fragmented MP4，`.m3u8` 输出为 HLS-fMP4。文件扩展名按当前实现
区分大小写。

## Pipeline 静态拓扑规则

- Input 至少连接一个 Sink，且固定输出 Packet。
- Encoder 可以没有媒体下游；其他非终端 Sink 至少连接一个；终端 Sink 不能配置媒体下游。
- 媒体边两端的 Packet/Frame 类型必须一致。
- 每个 Sink 只能有一个媒体上游；一个节点可以 fan-out 到多个下游，但不能汇聚。
- 所有 Sink 必须从 Input 可达，媒体拓扑不能存在环路。
- `downstream` 引用的 Sink ID 必须存在。
- 静态校验不检查实际轨道集合、媒体格式和编解码器可用性，这些条件在运行组件
  打开输入或编解码器时检查。

## 对应源码

- 公开配置结构：`include/mw/log.h`、`include/mw/streamer/*/config.h`、
  `include/mw/streamer/pipeline/pipeline_config.h`
- TOML 字段映射：`src/streamer/config/toml.cc`
- Pipeline 与 Sink 校验：`src/streamer/pipeline/pipeline_builder.cc`
- ZLM 参数校验：`src/streamer/zlm/internal/config_validator.cc`
- Remux 目标校验：`src/streamer/output/internal/remux_output.cc`
