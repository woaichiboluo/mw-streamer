# 上游来源

- 项目：ZLMediaKit
- 仓库：https://github.com/ZLMediaKit/ZLMediaKit.git
- 固定提交：`7563db36bb7002ce8601266c93b347f01a90b113`
- 提交说明：`fix: restore SRT transport cleanup on session error (#4777)`

本目录由上述提交裁剪得到。

HTTP 播放服务恢复同一固定提交中的 `HttpSession`、`HttpFileManager`、`HttpConst`、`HttpCookieManager`、`WebSocketSplitter` 及其必要 HTTP 配置和访问事件。监听仍由宿主显式创建，不恢复独立 MediaServer、REST API 或 WebRTC 产品功能；日志调用接入现有 `mw::log`，移除未使用的旧日志头文件。`HttpCookieManager` 接入现有 Runtime 的显式创建和实例安装流程，关闭时先取消过期定时器并释放 cookie 附件，再关闭线程池和时间戳时钟，避免 HTTP 播放状态跨越 Runtime 生命周期。

本地修正：`Common/Runtime.h` 提供统一的 `mediakit::init()`、`mediakit::shutdown()`，由 SDK 运行时管理网络、时间戳时钟、空媒体源、线程池和 SRT reactor。关闭时先停止所有工作线程，再移除空媒体源入口、销毁空源，最后释放时钟和网络，避免静态析构顺序依赖。日志通过 `mw::log` 输出，未配置时使用 spdlog 默认日志器；调用方在所有 ZLM 使用者销毁后关闭运行时。

HLS 直播清理延迟 `hls.deleteDelaySec` 默认设为 `0`，复用上游立即清理分支，使发布停止时同步清理播放列表与切片。
