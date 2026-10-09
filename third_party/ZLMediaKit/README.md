# ZLMediaKit 嵌入式协议内核裁剪版

本目录基于固定版本的 ZLMediaKit 源码裁剪，只作为 `mw-streamer` 的内部协议与媒体封装依赖，不提供独立流媒体服务器。
支持平台固定为 Linux 和 Windows；Linux 保留 x86_64/ARM，Windows 保留 MSVC 与 wepoll 事件循环。

## 保留能力

- RTMP 拉流与推流；检测到 OpenSSL 时同时支持 RTMPS。
- RTSP 拉流与推流；检测到 OpenSSL 时同时支持 RTSPS。
- RTSP 播放服务端会话；宿主可用 `toolkit::TcpServer::start<mediakit::RtspSession>()` 启动监听，将进程内注册的 RTSP 媒体源发布给客户端。
- SRT 拉流与推流。
- HTTP、HTTP-FLV、HTTP-TS、HLS 客户端能力；检测到 OpenSSL 时同时支持 HTTPS。
- HTTP 文件与 HLS 播放服务端会话；宿主显式创建 HTTP 监听，可将 HLS 切片以 `/app/stream/hls.m3u8` 地址发布给客户端。
- GB28181、裸 RTP、MPEG-PS over RTP 接入与转发。
- MP4、FMP4、FLV、MPEG-TS、MPEG-PS 封装与解封装。
- H.264、H.265、H.266、AV1、AAC、Opus、G711、MP3 等媒体格式适配。
- 网络线程、DNS、TLS、定时器、缓冲及协议重连所需基础设施。

## 已移除能力

- MediaServer 可执行程序及监听服务编排。
- RTMP、SRT 的普通入站服务端，以及 RTSP 推流入站的产品接入流程。
- REST Web API、WebHook、管理后台和 Swagger/Postman 资源。
- WebRTC、ONVIF、Shell、Go/C/Python 绑定和 SDL 播放器示例。
- Docker、Kubernetes、RPM 等独立服务器部署资源。
- Android 示例与旧版 NDK 工具链。
- macOS、iOS 平台实现与工具链。
- 未参与当前构建的第三方协议库、测试、CI 和工程文件。

## 构建

默认固定编译上述全部能力并生成静态库，不提供产品功能裁剪开关：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

OpenSSL 为自动探测的可选依赖：未安装时普通 RTMP、RTSP、SRT、HTTP、HLS 能力仍会编译，TLS URL 和带口令的 SRT 会明确拒绝。宿主应在创建 Player/Pusher 前完成线程池、配置和 TLS 策略初始化，并在退出时先销毁协议对象，再结束网络运行时。

RTSP 播放监听由宿主显式创建 `toolkit::TcpServer` 并启动 `mediakit::RtspSession`；本目录不提供独立服务进程或监听编排。`RtspSession`、`RtpMultiCaster` 和 `UDPServer` 来自固定上游提交，保留原有 RTSP over TCP、UDP 及组播传输处理。

HTTP 播放监听同样由宿主显式创建 `toolkit::TcpServer` 并启动原生 `mediakit::HttpSession`。HLS 发布复用原生 `MultiMediaSourceMuxer`、`HlsRecorder` 和 `HttpFileManager`，由 SDK 管理媒体源注册、切片、索引等待、文件发送及访问事件；HLS 保存目录与 `http.rootPath` 对齐，切片保留遵循原生配置；本 SDK 将 `hls.deleteDelaySec` 默认设为 `0`，停止直播时由原生清理逻辑立即删除播放列表及切片。无需启动独立 MediaServer 或 REST API。
