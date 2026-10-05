#include "mw/streamer/input/zlm_input.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <utility>

#include "Player/MediaPlayer.h"
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "mw/log.h"
#include "mw/streamer/converter/zlm_codec_parameters_converter.h"
#include "mw/streamer/converter/zlm_packet_converter.h"

namespace mw::streamer {
namespace {

template <typename Callback, typename... Args>
void InvokeCallback(const Callback& callback, Args&&... args) noexcept {
  if (!callback) {
    return;
  }
  try {
    callback(std::forward<Args>(args)...);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "Input回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "Input回调抛出了未知异常");
  }
}

}  // namespace

struct ZlmInput::TrackBinding {
  mediakit::Track::Ptr track;
  mediakit::FrameWriterInterface* delegate;
};

ZlmInput::ZlmInput(ZlmInputConfig config) : config_(std::move(config)) {
  if (config_.max_retries < -1 || config_.retry_interval.count() <= 0) {
    throw std::invalid_argument("Input重连配置无效");
  }
  poller_ = toolkit::EventPollerPool::Instance().getPoller();
}

ZlmInput::~ZlmInput() {
  poller_->sync([this]() {
    ClearAttempt();
    state_.store(InputState::kStopped);
  });
}

void ZlmInput::SetOnReady(OnReady callback) { on_ready_ = std::move(callback); }

void ZlmInput::SetOnPacket(OnPacket callback) {
  on_packet_ = std::move(callback);
}

void ZlmInput::SetOnStateChanged(OnStateChanged callback) {
  on_state_changed_ = std::move(callback);
}

void ZlmInput::Start(std::string_view url) {
  if (url.empty() || std::all_of(url.begin(), url.end(), [](unsigned char ch) {
        return std::isspace(ch);
      })) {
    throw std::invalid_argument("Input URL不能为空");
  }
  std::lock_guard<std::mutex> lock(control_mutex_);
  const auto current = state();
  if (current == InputState::kConnecting || current == InputState::kConnected ||
      current == InputState::kWaitingRetry) {
    throw std::logic_error("Input已经启动");
  }
  std::string owned_url(url);
  state_.store(InputState::kConnecting);
  poller_->async(
      [this, url = std::move(owned_url)]() mutable {
        ClearAttempt();
        url_ = std::move(url);
        retry_count_ = 0;
        BeginAttempt();
      },
      false);
}

void ZlmInput::Stop() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (state() == InputState::kStopped) {
    return;
  }
  poller_->sync([this]() {
    ClearAttempt();
    NotifyState(InputState::kStopped);
  });
}

InputState ZlmInput::state() const noexcept { return state_.load(); }

void ZlmInput::BeginAttempt() {
  retry_timer_.reset();
  attempt_active_ = std::make_shared<std::atomic<bool>>(true);
  const auto active = attempt_active_;
  player_ = std::make_shared<mediakit::MediaPlayer>(poller_);
  player_->setOnPlayResult([this, active](const toolkit::SockException& error) {
    if (active->load()) {
      HandlePlayResult(active, error.getErrCode(), error.what());
    }
  });
  player_->setOnShutdown([this, active](const toolkit::SockException& error) {
    if (!active->load()) {
      return;
    }
    // Defer teardown until the ZLM callback has returned.
    const bool ended =
        error.getErrCode() == toolkit::Err_eof && player_->isFinite();
    if (ended) {
      poller_->async(
          [this, active]() {
            if (active->load()) {
              HandleEnd();
            }
          },
          false);
    } else {
      QueueFailure(active, error.getErrCode(), error.what(),
                   !player_->isFinite());
    }
  });
  NotifyState(InputState::kConnecting);
  try {
    player_->play(url_);
  } catch (const std::exception& error) {
    QueueFailure(active, toolkit::Err_other, error.what(), false);
  }
}

void ZlmInput::HandlePlayResult(
    const std::shared_ptr<std::atomic<bool>>& active, int error_code,
    std::string_view message) {
  if (error_code != toolkit::Err_success) {
    QueueFailure(active, error_code, std::string(message),
                 !player_->isFinite());
    return;
  }
  try {
    const auto tracks = player_->getTracks(true);
    if (tracks.empty()) {
      throw std::runtime_error("Input没有就绪轨道");
    }
    streams_.reserve(tracks.size());
    converters_.reserve(tracks.size());
    bindings_.reserve(tracks.size());
    for (const auto& track : tracks) {
      const auto index = static_cast<int>(streams_.size());
      internal::ZlmCodecParametersConverter parameters(track);
      streams_.push_back(
          {index, parameters.codec_parameters(), parameters.time_base()});
      auto converter =
          std::make_shared<internal::ZlmPacketConverter>(track, index);
      converter->SetOnPacket([this, active](const ffmpeg::Packet& packet) {
        if (!active->load()) {
          return false;
        }
        InvokeCallback(on_packet_, packet);
        return true;
      });
      converters_.push_back(std::move(converter));
    }
    retry_count_ = 0;
    NotifyState(InputState::kConnected);
    {
      std::lock_guard<std::mutex> lock(delivery_mutex_);
      InvokeCallback(on_ready_, streams_);
    }
    for (std::size_t index = 0; index < tracks.size(); ++index) {
      auto converter = converters_[index];
      auto* delegate = tracks[index]->addDelegate(
          [this, active, converter](const mediakit::Frame::Ptr& frame) {
            if (!active->load()) {
              return false;
            }
            std::lock_guard<std::mutex> lock(delivery_mutex_);
            if (!active->load()) {
              return false;
            }
            try {
              return converter->InputFrame(frame);
            } catch (const std::exception& error) {
              QueueFailure(active, toolkit::Err_other, error.what(), false);
              return false;
            }
          });
      bindings_.push_back({tracks[index], delegate});
    }
  } catch (const std::exception& error) {
    QueueFailure(active, toolkit::Err_other, error.what(), false);
  }
}

void ZlmInput::QueueFailure(const std::shared_ptr<std::atomic<bool>>& active,
                            int error_code, std::string message, bool retry) {
  poller_->async(
      [this, active, error_code, message = std::move(message), retry]() {
        if (active->load()) {
          HandleFailure(error_code, message, retry);
        }
      },
      false);
}

void ZlmInput::HandleFailure(int error_code, std::string_view message,
                             bool retry) {
  ClearAttempt();
  if (!retry || !config_.auto_reconnect ||
      (config_.max_retries >= 0 && retry_count_ >= config_.max_retries)) {
    NotifyState(InputState::kFailed, error_code, message);
    return;
  }
  ++retry_count_;
  NotifyState(InputState::kWaitingRetry, error_code, message);
  retry_timer_ = std::make_shared<toolkit::Timer>(
      std::chrono::duration<float>(config_.retry_interval).count(),
      [this]() {
        BeginAttempt();
        return false;
      },
      poller_);
}

void ZlmInput::HandleEnd() {
  try {
    std::lock_guard<std::mutex> lock(delivery_mutex_);
    for (const auto& converter : converters_) {
      converter->Flush();
    }
  } catch (const std::exception& error) {
    HandleFailure(toolkit::Err_other, error.what(), false);
    return;
  }
  ClearAttempt();
  NotifyState(InputState::kEnded);
}

void ZlmInput::ClearAttempt() {
  retry_timer_.reset();
  if (attempt_active_) {
    attempt_active_->store(false);
  }
  // Do not hold delivery_mutex_ while removing delegates: dispatch holds the
  // track lock before acquiring delivery_mutex_. Removal waits for delivery.
  for (const auto& binding : bindings_) {
    binding.track->delDelegate(binding.delegate);
  }
  bindings_.clear();
  converters_.clear();
  streams_.clear();
  if (player_) {
    player_->setOnPlayResult({});
    player_->setOnShutdown({});
    player_->teardown();
    player_.reset();
  }
  attempt_active_.reset();
}

void ZlmInput::NotifyState(InputState state, int error_code,
                           std::string_view message) {
  state_.store(state);
  std::lock_guard<std::mutex> lock(delivery_mutex_);
  InvokeCallback(on_state_changed_, state, error_code, message);
}

}  // namespace mw::streamer
