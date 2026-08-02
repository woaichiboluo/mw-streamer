# 真实推拉流端到端测试

本目录通过 `StreamingPipeline` 使用真实的 FFmpeg、MediaMTX 和媒体文件验证以下
能力：

- `0s / 1s / 5s / 15s / 30s` 压缩包缓存边界与代表值；
- RTSP、RTMP、SRT 稳定拉流；
- 输入连接被服务端主动断开后的自动重连；
- RTSP、RTMP、SRT 稳定推流；
- 输出连接被服务端主动断开后的自动重连；
- 同一输入同时稳定推送到三个协议；
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
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target mw_streamer_e2e_runner -j
```

如果机器同时安装了多套 FFmpeg，应在配置 CMake 时通过
`FFmpeg_ROOT` 明确选择与项目一致的版本：

```bash
cmake -S . -B build -DBUILD_TESTING=ON \
  -DFFmpeg_ROOT=/absolute/path/to/ffmpeg
```

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

Runner 始终配置至少一个真实输出目标。Pipeline 是否成功由其公共状态事件判断，
媒体轨道和持续输出则由 FFmpeg 从输出端实际读取验证；测试不再依赖 PlayerProxy
或 PacketQueue 的内部状态。

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
