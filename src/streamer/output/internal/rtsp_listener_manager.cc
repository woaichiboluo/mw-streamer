#include "output/internal/rtsp_listener_manager.h"

#include <fmt/format.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#include "Network/TcpServer.h"
#include "Rtsp/RtspSession.h"

namespace mw::streamer::internal {
namespace {

using Endpoint = std::pair<std::string, std::uint16_t>;
using Path = std::pair<std::string, std::string>;

struct ListenerEntry {
  std::shared_ptr<toolkit::TcpServer> server;
  std::set<Path> paths;
};

struct ListenerRegistry {
  std::mutex mutex;
  std::map<Endpoint, ListenerEntry> listeners;
};

ListenerRegistry& Registry() {
  static ListenerRegistry registry;
  return registry;
}

}  // namespace

RtspListenerLease::RtspListenerLease(std::string bind_ip, std::uint16_t port,
                                     std::string app, std::string stream)
    : bind_ip_(std::move(bind_ip)),
      port_(port),
      app_(std::move(app)),
      stream_(std::move(stream)) {}

RtspListenerLease::~RtspListenerLease() { Release(); }

RtspListenerLease::RtspListenerLease(RtspListenerLease&& other) noexcept
    : bind_ip_(std::move(other.bind_ip_)),
      port_(other.port_),
      app_(std::move(other.app_)),
      stream_(std::move(other.stream_)),
      active_(std::exchange(other.active_, false)) {}

RtspListenerLease& RtspListenerLease::operator=(
    RtspListenerLease&& other) noexcept {
  if (this == &other) return *this;
  Release();
  bind_ip_ = std::move(other.bind_ip_);
  port_ = other.port_;
  app_ = std::move(other.app_);
  stream_ = std::move(other.stream_);
  active_ = std::exchange(other.active_, false);
  return *this;
}

RtspListenerLease RtspListenerLease::Acquire(std::string bind_ip,
                                             std::uint16_t port,
                                             std::string app,
                                             std::string stream) {
  if (bind_ip.empty() || port == 0 || app.empty() || stream.empty() ||
      app.find('/') != std::string::npos ||
      stream.find('/') != std::string::npos) {
    throw std::invalid_argument("RTSP监听地址、端口、app或stream无效");
  }

  RtspListenerLease lease(std::move(bind_ip), port, std::move(app),
                          std::move(stream));
  const Endpoint endpoint{lease.bind_ip_, lease.port_};
  const Path path{lease.app_, lease.stream_};
  auto& registry = Registry();
  std::lock_guard lock(registry.mutex);

  // ZLM's media-source registry is global, even when listeners use different
  // ports. Publishing one path twice would replace the source unexpectedly.
  for (const auto& [other_endpoint, entry] : registry.listeners) {
    if (entry.paths.count(path) != 0) {
      throw std::invalid_argument(
          fmt::format("RTSP发布路径已占用: {}/{}", lease.app_, lease.stream_));
    }
  }

  auto entry = registry.listeners.find(endpoint);
  if (entry == registry.listeners.end()) {
    auto server = std::make_shared<toolkit::TcpServer>();
    try {
      server->start<mediakit::RtspSession>(port, lease.bind_ip_);
    } catch (const std::exception& error) {
      throw std::runtime_error(fmt::format("RTSP监听 {}:{} 失败: {}",
                                           lease.bind_ip_, port, error.what()));
    }
    entry = registry.listeners
                .emplace(endpoint, ListenerEntry{std::move(server), {}})
                .first;
  }
  try {
    entry->second.paths.insert(path);
  } catch (...) {
    if (entry->second.paths.empty()) registry.listeners.erase(entry);
    throw;
  }
  lease.active_ = true;
  return lease;
}

void RtspListenerLease::Release() noexcept {
  if (!active_) return;
  auto& registry = Registry();
  std::lock_guard lock(registry.mutex);
  const auto entry = registry.listeners.find({bind_ip_, port_});
  if (entry != registry.listeners.end()) {
    entry->second.paths.erase({app_, stream_});
    if (entry->second.paths.empty()) registry.listeners.erase(entry);
  }
  active_ = false;
}

}  // namespace mw::streamer::internal
