#include "mw/streamer/input/ffmpeg_packet_reader.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::internal {

FfmpegPacketReader::FfmpegPacketReader(Read read, CancelRead cancel_read,
                                       std::size_t byte_limit)
    : read_(std::move(read)),
      cancel_read_(std::move(cancel_read)),
      byte_limit_(byte_limit) {
  if (!read_ || !cancel_read_ || !byte_limit_) {
    throw std::invalid_argument("Packet 读取器配置无效");
  }
  worker_ = std::thread(&FfmpegPacketReader::Run, this);
}

FfmpegPacketReader::~FfmpegPacketReader() { Stop(); }

bool FfmpegPacketReader::Pop(ffmpeg::Packet& packet) {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] {
    return stopping_ || !packets_.empty() || finished_ ||
           pending_bytes_ > byte_limit_ - outstanding_bytes_;
  });
  if (stopping_) return false;
  if (!packets_.empty()) {
    packet = std::move(packets_.front());
    packets_.pop_front();
    lock.unlock();
    changed_.notify_all();
    return true;
  }
  if (error_) std::rethrow_exception(error_);
  if (pending_bytes_ > byte_limit_ - outstanding_bytes_) {
    throw std::length_error("Input 轨道 Packet 缓存超过字节上限");
  }
  return false;
}

bool FfmpegPacketReader::TryPop(ffmpeg::Packet& packet) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return false;
  if (packets_.empty()) {
    if (error_) std::rethrow_exception(error_);
    return false;
  }
  packet = std::move(packets_.front());
  packets_.pop_front();
  lock.unlock();
  changed_.notify_all();
  return true;
}

bool FfmpegPacketReader::finished() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_ && packets_.empty() && !error_;
}

bool FfmpegPacketReader::budget_blocked() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return packets_.empty() && pending_bytes_ > byte_limit_ - outstanding_bytes_;
}

void FfmpegPacketReader::Release(std::size_t bytes) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    outstanding_bytes_ -= std::min(bytes, outstanding_bytes_);
  }
  changed_.notify_all();
}

void FfmpegPacketReader::RequestStop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  changed_.notify_all();
  try {
    cancel_read_();
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "Packet 读取器取消回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "Packet 读取器取消回调抛出了未知异常");
  }
}

void FfmpegPacketReader::Stop() noexcept {
  RequestStop();
  if (worker_.joinable()) worker_.join();
}

std::size_t FfmpegPacketReader::outstanding_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_bytes_;
}

std::size_t FfmpegPacketReader::high_water_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return high_water_bytes_;
}

void FfmpegPacketReader::Run() noexcept {
  try {
    int consecutive_enomem = 0;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) break;
      }
      ffmpeg::Packet packet;
      const int result = read_(packet);
      if (result == AVERROR_EXIT || result == AVERROR_EOF) break;
      if (result == AVERROR(EAGAIN)) {
        std::this_thread::yield();
        continue;
      }
      if (result == AVERROR(ENOMEM)) {
        ++consecutive_enomem;
        if (consecutive_enomem == 1) {
          MW_LOG_WARNING("streamer",
                         "Packet 读取器 av_read_frame 内存不足，稍后重试");
        } else if (consecutive_enomem >= 20) {
          MW_LOG_ERROR("streamer",
                       "Packet 读取器 av_read_frame 连续内存不足，结束读取");
          break;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (changed_.wait_for(lock, std::chrono::milliseconds(5),
                              [this] { return stopping_; })) {
          break;
        }
        continue;
      }
      ffmpeg::FfmpegException::throwIfError(result, "av_read_frame");
      consecutive_enomem = 0;
      if (packet->size <= 0) continue;
      const auto bytes = static_cast<std::size_t>(packet->size);
      if (bytes > byte_limit_) {
        throw std::length_error("Input 轨道 Packet 缓存超过字节上限");
      }
      std::unique_lock<std::mutex> lock(mutex_);
      pending_bytes_ = bytes;
      changed_.notify_all();
      changed_.wait(lock, [this, bytes] {
        return stopping_ || (packets_.size() < kPacketCapacity &&
                             bytes <= byte_limit_ - outstanding_bytes_);
      });
      pending_bytes_ = 0;
      if (stopping_) break;
      packets_.push_back(std::move(packet));
      outstanding_bytes_ += bytes;
      high_water_bytes_ = std::max(high_water_bytes_, outstanding_bytes_);
      lock.unlock();
      changed_.notify_all();
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::current_exception();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_bytes_ = 0;
    finished_ = true;
  }
  changed_.notify_all();
}

}  // namespace mw::streamer::internal
