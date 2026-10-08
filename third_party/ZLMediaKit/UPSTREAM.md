# 上游来源

- 项目：ZLMediaKit
- 仓库：https://github.com/ZLMediaKit/ZLMediaKit.git
- 固定提交：`7563db36bb7002ce8601266c93b347f01a90b113`
- 提交说明：`fix: restore SRT transport cleanup on session error (#4777)`

本目录由上述提交裁剪得到。

本地修正：`Common/Runtime.h` 提供统一的 `mediakit::init()`、`mediakit::shutdown()`，由 SDK 运行时管理网络、时间戳时钟、空媒体源、线程池和 SRT reactor。关闭时先停止所有工作线程，再移除空媒体源入口、销毁空源，最后释放时钟和网络，避免静态析构顺序依赖。日志通过 `mw::log` 输出，未配置时使用 spdlog 默认日志器；调用方在所有 ZLM 使用者销毁后关闭运行时。
