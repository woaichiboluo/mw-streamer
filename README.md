# mw-streamer

项目正在重建，目前仅保留独立的日志模块及最小构建骨架。

- `src/log`：日志实现，依赖 fmt 和 spdlog。
- `include/mw/log.h`：C 日志接口和支持 fmt 格式化的 C++ 日志宏。
- `include/mw/export.h`：日志库的导出宏。
- `third_party`：保留已有第三方源码，暂不参与构建。
- `cmake`：依赖版本定义及保留的 FFmpeg、SRT 查找模块。

旧的流媒体实现、公开接口、配置模板、示例、测试和安装打包逻辑已移除。
当前构建不需要 FFmpeg、SRT、OpenSSL、CUDA 或 OpenCV。

## 构建

要求 CMake 3.22 或更新版本，以及支持 C++17 的编译器。fmt 和 spdlog
按固定版本获取，首次配置需要网络。Windows 使用 Visual Studio x64 开发者终端。

```sh
cmake -S . -B build/log -DCMAKE_BUILD_TYPE=Release
cmake --build build/log --config Release --parallel
```

默认生成静态日志库；需要动态库时，在配置阶段添加 `-DBUILD_SHARED=ON`。
作为子工程使用时，通过 `add_subdirectory` 引入，链接目标 `mw::log`。

## 日志

```cpp
#include "mw/log.h"

int main() {
    MwLogConfig config;
    mw_log_default_config(&config);
    if (mw_log_initialize(&config) != kMwLogSuccess) {
        return 1;
    }
    MW_LOG_INFO_DEFAULT("日志模块已启动，版本 {}", 1);
    mw_log_shutdown();
}
```

保留原有模块级别过滤、控制台输出、滚动文件及异步日志功能。
