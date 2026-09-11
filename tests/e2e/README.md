# 真实推拉流端到端测试

本目录通过新的 `pipeline::Pipeline` 使用真实的 FFmpeg、MediaMTX 和媒体文件验证以下
能力。运行器只在组装时依赖 `pipeline`；Input、Sink 和具体处理节点使用各自模块的
公开接口：`mw/input/`、`mw/sink/`、`mw/decoder/`、`mw/processor/`、
`mw/synchronizer/`、`mw/encoder/` 和 `mw/output/`。模块名也是
`mw::streamer` 下的命名空间。共用投递参数位于 `media`，输入状态位于 `input`，
消息和 Fatal 位于 `sink`；节点参数来自各自的 `config.h`。测试观察节点继承
`sink::Sink`，通过 `media::PacketReady`、`media::FrameReady` 等参数接收数据。

运行器按测试场景构建不同的 Sink 链路，全部使用同一个 Pipeline 类型：

- `0s / 1s / 5s / 15s / 30s` 压缩包缓存边界与代表值；
- RTSP、RTMP、SRT 稳定拉流；
- 输入连接被服务端主动断开后的自动重连；
- Processor Fatal 错误自动停止链路，运行器报告失败并返回非零退出码；
- RTSP、RTMP、SRT 稳定推流；
- 输出连接被服务端主动断开后的自动重连；
- 同一输入同时稳定推送到三个协议；
- 同步后的 Frame 同时交给本地观察 Sink 和 EncoderSink，编码目标覆盖 FILE、RTMP、
  RTSP、SRT；
- Input 连接多个独立 RemuxSink，将源压缩流同时转推并录制为本地 MP4；
- 原流 Remux 旁路与解码、处理、同步、编码链路同时运行；
- FileInput 全速读取本地文件，经 DecoderSink 和 AnalysisProcessorSink 验证自然 EOF、
  音视频计数及 Processor 生命周期；
- 多推过程中单个输出故障后的隔离和恢复。
- H.264/H.265 专用白闪与音频脉冲媒体的内容级音画同步；
- FILE、RTMP、RTSP、SRT 输入到 FILE、RTMP、RTSP、SRT 输出的完整同步矩阵；每种输入在一次 Pipeline 中同时输出四种目标，每组连续运行 5 分钟；
- 四种输入在视频偶发阻塞 100～200ms 时的同步边界；
- 断线备播恢复后的同步边界。

同步测试确定性生成 330 秒专用媒体。所有输出统一忽略前 15 秒起播阶段，之后对
完整稳定窗口中的每组内容标记检查绝对音画偏差，声音领先或滞后均不得超过 40ms，
不进行起播偏移归一化。五分钟长稳矩阵共执行 H.264/H.265 × 四种输入 8 组，
每组同时检查 FILE、RTMP、RTSP、SRT 四份输出，串行运行约 40 分钟。
RTMP、SRT 输出由对应协议的独立 MediaMTX sink 在入站侧直接录制；RTSP 输出由
FFmpeg 的 RTSP RECORD 监听端直接录制，避免 MediaMTX 默认按音视频首包到达时间
分别重建 NTP 后引入固定起播偏移。宿主机系统时钟调整可能使 MediaMTX 自动切分
录像，测试会无损拼接全部分段，并且只在完整录像开头忽略一次 15 秒。

测试不使用 Docker。常规协议矩阵使用配置目录中的真实媒体；音画同步测试会确定性
生成带白闪与音频脉冲的专用媒体。每个常规测试媒体可以是纯音频、纯视频或同时包含
音视频。收集测试时会使用 `ffprobe` 检查目录中的每个文件；无法识别或不满足首版
编解码范围的文件会直接报错，不会静默跳过。视频尺寸和平均帧率同样由 `ffprobe`
读取，并作为 Pipeline 的固定输出参数。

## 准备配置

复制模板并填写本机绝对路径：

```bash
cp tests/e2e/e2e.example.toml tests/e2e/e2e.local.toml
```

`e2e.local.toml` 和 `artifacts/` 已被 Git 忽略。模板中的工具路径分别指向 FFmpeg、
FFprobe 和 MediaMTX，媒体配置只需要填写一个目录。

## 构建运行器

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target mw_streamer_e2e_runner -j
```

FFmpeg 和 SRT 自动接受平台能够找到的共享库或静态库。FFmpeg 必须为
5.0 或更高版本，并安装
`pkg-config` 或兼容的 `pkgconf`，并为所用组件提供 `.pc` 文件。依赖位于非标准前缀
或机器同时安装了多套依赖时，统一通过 `FFMPEG_ROOT`、`SRT_ROOT` 和
`OPENSSL_ROOT` 指定各自的安装根目录：

```bash
cmake -S . -B build -DBUILD_TESTS=ON \
  -DFFMPEG_ROOT=/absolute/path/to/ffmpeg \
  -DSRT_ROOT=/absolute/path/to/srt \
  -DOPENSSL_ROOT=/absolute/path/to/openssl
```

也可以使用 CMake 通用的 `CMAKE_PREFIX_PATH` 提供查找前缀。FFmpeg 各组件的 `.pc`
文件仍须位于对应安装根目录的标准 pkg-config 目录中，或可由 `PKG_CONFIG_PATH` 找到。

## 运行测试

```bash
python3 -m venv .cache/e2e-venv
.cache/e2e-venv/bin/pip install -r tests/e2e/requirements.txt
.cache/e2e-venv/bin/python -m pytest -c tests/e2e/pytest.ini tests/e2e \
  --e2e-config tests/e2e/e2e.local.toml \
  --e2e-runner build/tests/e2e/mw_streamer_e2e_runner
```

可以使用标记缩小范围：

```bash
# 基础拉流
.cache/e2e-venv/bin/python -m pytest -c tests/e2e/pytest.ini tests/e2e \
  -m smoke --e2e-config tests/e2e/e2e.local.toml \
  --e2e-runner build/tests/e2e/mw_streamer_e2e_runner

# 断线重连
.cache/e2e-venv/bin/python -m pytest -c tests/e2e/pytest.ini tests/e2e \
  -m fault --e2e-config tests/e2e/e2e.local.toml \
  --e2e-runner build/tests/e2e/mw_streamer_e2e_runner
```

运行器的 `--scenario streaming|remux|file` 只选择测试链路：

- `streaming`：ZlmInput → DecoderSink → TransformProcessorSink →
  SynchronizerSink → EncoderSink → 每个输出目标各自的 RemuxSink。本地观察 Sink
  接在 SynchronizerSink 后；原流输出直接接在 Input 后。没有处理后输出和本地观察
  Sink 时，使用 AnalysisProcessorSink，避免创建无消费者的编码节点。
- `remux`：ZlmInput → 每个输出目标各自的 RemuxSink，至少需要一个目标。
- `file`：FileInput → 软件 DecoderSink → AnalysisProcessorSink。FileInput 通过
  FFmpeg 全速读取文件，解码队列有界等待，不主动丢包；保留 Skip Samples 等
  packet side data，两份 AAC 样本精确验证 94 帧、96256 个音频样本。
  等待解码排空后停止 Pipeline；输入 EOF 本身不代表下游已经完成。
  音视频处理回调可并发。此场景验证帧数和结束边界，不提供文件进度或媒体处理倍速。

SynchronizerSink 自带实时调度和备播，不再使用旧 `--standby` 开关。测试环境的
`e2e.local.toml` 仍用于配置工具、媒体和测试时长，与库的 Pipeline TOML 用途不同。

运行器检查 Pipeline、输入及各工作 Sink 的状态；实际网络输出仍由媒体服务和 FFmpeg
探针验证。缓存测试通过 `--observe-cache` 加入同步转发的测试观察 Sink，比较每个轨道
输入最新 DTS 与首个解码帧 PTS 的媒体时间差；播放器启动时批量交付积累的帧，不代表
缓存必须再等待等长的墙钟时间。0 缓存仍检查首帧启动延迟。
`runner_started` 的 `pipeline_api=unified` 标记用于防止测试误用旧版本运行器。

`performance` 事件来自 `Pipeline::GetPerformance()`，按 `node_id` 和固定 `type`
区分节点及音视频操作。运行期间通过相邻快照计算速率，停止后输出 `phase=final`
的累计计数。输入旁路不会产生编码统计；Remux 的计数表示本地封装处理，不代表远端
已经收到数据。`summary` 保留测试需要的汇总字段，其编解码和处理计数来自新快照。

## 运行十分钟 Bench

Bench 分别选择媒体目录中像素数最小的 H.264 和 H.265 真实视频。每种编码分别
通过 FILE、RTSP、RTMP、SRT 输入，并在一次 Pipeline 编码后同时推送 RTSP、
RTMP、SRT。每组持续 10 分钟，共 8 组，串行执行约 80 分钟。网络输入通过
FFmpeg 循环发布；FILE 输入则在用例开始前通过压缩包复制生成约 11 分钟的临时
MP4，因此两种输入都会运行多轮真实媒体，且不会重新编码测试源。

Bench 默认跳过，需要显式启用：

```bash
.cache/e2e-venv/bin/python -m pytest \
  -c tests/e2e/pytest.ini \
  -m bench \
  --run-bench \
  --e2e-config tests/e2e/e2e.local.toml \
  --e2e-runner build/tests/e2e/mw_streamer_e2e_runner
```

每个目标都必须先由对应 MediaMTX sink 确认发布路径可用，分别证明 RTSP、RTMP、
SRT 推流成功。三个 FFmpeg 探针再从各 sink 的 RTSP 出口读取完整轨道，并使用
压缩包复制避免探针解码高分辨率视频干扰 Pipeline 性能。这也避开 RTMP reader
无法暴露 H.265 视频轨道的协议限制。如果任一路媒体进度连续超过启动超时时间没有
增长，或十分钟内累计媒体时间不足墙上时间的 98%，测试立即失败。媒体目录缺少
H.264 或 H.265 视频时也会明确报错。

每次运行的 MediaMTX 配置、进程日志、runner 事件和 FFmpeg progress 都保存在
`tests/e2e/artifacts/<UTC 时间>/`，失败后可以直接按测试用例目录定位证据。

推流用例会按目标协议连接对应的独立 MediaMTX，并统一通过该实例的 RTSP 观察口
读取媒体。这样既能检查实际推流协议的连接、重连和隔离，又不会把 RTMP 播放端对
H.265 等编码的兼容范围误算成推流端丢轨。
