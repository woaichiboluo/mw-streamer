#define WIN32_LEAN_AND_MEAN
#include <fmt/format.h>
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string Utf8(const wchar_t* value) {
  const auto size =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

int Child(const wchar_t* fixture_path, const wchar_t* sample_path,
          const wchar_t* log_path) {
  using RunLifecycle = int (*)(const char*, const char*);
  const auto sample = Utf8(sample_path);
  for (int iteration = 0; iteration < 3; ++iteration) {
    const auto iteration_log =
        std::wstring(log_path) + L"." + std::to_wstring(iteration);
    std::filesystem::remove(iteration_log);
    const auto log = Utf8(iteration_log.c_str());
    const auto fixture = LoadLibraryW(fixture_path);
    if (!fixture) {
      fmt::print(stderr, "LoadLibrary failed: {}\n", GetLastError());
      return 1;
    }
    const auto run =
        reinterpret_cast<RunLifecycle>(GetProcAddress(fixture, "RunLifecycle"));
    const bool passed = run && run(sample.c_str(), log.c_str()) == 0;
    const bool streamer_loaded =
        GetModuleHandleW(L"mw_streamer.dll") != nullptr;
    const bool unloaded = FreeLibrary(fixture) != 0;
    if (!passed || !streamer_loaded || !unloaded ||
        GetModuleHandleW(L"mw_streamer.dll") != nullptr ||
        GetModuleHandleW(L"mw_log.dll") != nullptr) {
      fmt::print(stderr, "DLL lifecycle/unload failed at iteration {}\n",
                 iteration);
      return 1;
    }
    std::ifstream output(std::filesystem::path(iteration_log),
                         std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(output), {});
    if (contents.find("DLL lifecycle completed") == std::string::npos) {
      fmt::print(stderr, "Asynchronous log was not flushed before unload\n");
      return 1;
    }
  }
  return 0;
}

std::wstring Quote(const wchar_t* value) {
  return L"\"" + std::wstring(value) + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc == 5 && std::wstring(argv[1]) == L"--child") {
    return Child(argv[2], argv[3], argv[4]);
  }
  if (argc != 4) {
    fmt::print(stderr, "Usage: unload_host FIXTURE_DLL SAMPLE_MP4 LOG_PATH\n");
    return 2;
  }
  wchar_t executable[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
    return 2;
  }
  auto command = Quote(executable) + L" --child " + Quote(argv[1]) + L" " +
                 Quote(argv[2]) + L" " + Quote(argv[3]);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    fmt::print(stderr, "Cannot launch unload subprocess: {}\n", GetLastError());
    return 2;
  }
  const auto waited = WaitForSingleObject(process.hProcess, 40000);
  DWORD exit_code = 1;
  if (waited == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &exit_code);
  } else {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 1000);
    fmt::print(stderr,
               "DLL unload subprocess did not complete within 40 seconds\n");
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}
