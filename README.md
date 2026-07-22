# mw-streamer

`mw-streamer` 集成了一份固定版本的轻量 ZLMediaKit 源码，用于后续构建可嵌入业务进程的流媒体静态库。

当前协议核心保留：

- RTMP、RTSP、SRT、HTTP-FLV、HTTP-TS、HLS 拉流。
- RTMP、RTSP、SRT 推流。
- MP4、FMP4、FLV、MPEG-TS、MPEG-PS 封装与解封装能力。
- Linux 和 Windows 网络运行时。
- 可选 OpenSSL；未检测到 OpenSSL 时仍编译普通非 TLS 协议。

服务端主程序、Web API、WebRTC、语言绑定、移动端工程和上游测试未保留。当前源码不依赖 JSON。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

构建产物均为静态库。ZLMediaKit 固定源码位于 `third_party/ZLMediaKit`，其上游版本和裁剪边界记录在该目录的文档及 Git 历史中。
