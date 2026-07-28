# 真实推拉流端到端测试

本目录使用真实的 FFmpeg、MediaMTX 和媒体文件验证以下能力：

- `1s / 5s / 15s / 30s` PacketQueue 缓存边界与代表值；
- RTSP、RTMP、SRT 稳定拉流；
- 输入连接被服务端主动断开后的自动重连；
- RTSP、RTMP、SRT 稳定推流；
- 输出连接被服务端主动断开后的自动重连；
- 同一输入同时稳定推送到三个协议；
- 多推过程中单个输出故障后的隔离和恢复。

测试不使用 Docker，也不生成伪媒体。每个测试媒体可以是纯音频、纯视频或同时包含
音视频。收集测试时会使用 `ffprobe` 检查目录中的每个文件；无法识别或不满足首版
编解码范围的文件会直接报错，不会静默跳过。

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

每次运行的 MediaMTX 配置、进程日志、runner 事件和 FFmpeg progress 都保存在
`tests/e2e/artifacts/<UTC 时间>/`，失败后可以直接按测试用例目录定位证据。

推流用例会按目标协议连接对应的独立 MediaMTX，并统一通过该实例的 RTSP 观察口
读取媒体。这样既能检查实际推流协议的连接、重连和隔离，又不会把 RTMP 播放端对
H.265 等编码的兼容范围误算成推流端丢轨。
