#include "mw/streamer/input/ffmpeg_frame_decoder.h"

#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::internal {

FfmpegFrameDecoder::FfmpegFrameDecoder(std::unique_ptr<ffmpeg::Decoder> decoder,
                                       Release release, Notify notify,
                                       ParserRepeatPict parser_repeat_pict)
    : decoder_(std::move(decoder)),
      release_(std::move(release)),
      notify_(std::move(notify)),
      parser_repeat_pict_(std::move(parser_repeat_pict)) {
  if (!decoder_ || !release_ || !notify_) {
    throw std::invalid_argument("异步解码器配置无效");
  }
  worker_ = std::thread(&FfmpegFrameDecoder::Run, this);
}

FfmpegFrameDecoder::~FfmpegFrameDecoder() { Stop(); }

bool FfmpegFrameDecoder::Push(ffmpeg::Packet packet) {
  const auto bytes = packet.get() && packet->size > 0
                         ? static_cast<std::size_t>(packet->size)
                         : 0;
  if (!packet.get() || !packet->data || !bytes) {
    ReleasePacket(bytes, false);
    throw std::invalid_argument(
        "解码输入 Packet 不能为空，请使用 Finish 提交 EOF");
  }
  bool accepted = false;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_ && !input_finished_ && !worker_finished_) {
      packets_.push_back(std::move(packet));
      queued_bytes_ += bytes;
      accepted = true;
    }
  } catch (...) {
    ReleasePacket(bytes, false);
    throw;
  }
  if (!accepted) {
    ReleasePacket(bytes, false);
  }
  changed_.notify_all();
  return accepted;
}

void FfmpegFrameDecoder::Finish() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    input_finished_ = true;
  }
  changed_.notify_all();
}

bool FfmpegFrameDecoder::TryReceive(ffmpeg::Frame& frame) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return false;
  if (!frames_.empty()) {
    frame = std::move(frames_.front());
    frames_.pop_front();
    lock.unlock();
    changed_.notify_all();
    return true;
  }
  if (error_) std::rethrow_exception(error_);
  return false;
}

void FfmpegFrameDecoder::ThrowIfFailed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (error_ && frames_.empty()) std::rethrow_exception(error_);
}

void FfmpegFrameDecoder::RequestStop() noexcept {
  std::deque<ffmpeg::Packet> discarded;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    end_pts_ = AV_NOPTS_VALUE;
    discarded.swap(packets_);
    frames_.clear();
  }
  changed_.notify_all();
  for (const auto& packet : discarded) {
    ReleasePacket(packet.get() && packet->size > 0
                      ? static_cast<std::size_t>(packet->size)
                      : 0);
  }
  NotifyChanged();
}

void FfmpegFrameDecoder::Stop() noexcept {
  RequestStop();
  if (worker_.joinable()) worker_.join();
}

void FfmpegFrameDecoder::Restart() {
  Stop();
  decoder_->Flush();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    input_finished_ = false;
    worker_finished_ = false;
    stopping_ = false;
    waiting_for_input_ = false;
    end_pts_ = AV_NOPTS_VALUE;
    error_ = nullptr;
  }
  worker_ = std::thread(&FfmpegFrameDecoder::Run, this);
}

bool FfmpegFrameDecoder::finished() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopping_ || (worker_finished_ && frames_.empty() && !error_);
}

std::int64_t FfmpegFrameDecoder::end_pts() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return end_pts_;
}

bool FfmpegFrameDecoder::wants_packet() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && !input_finished_ && !worker_finished_ &&
         packets_.size() < kPacketWatermark;
}

bool FfmpegFrameDecoder::blocked() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && !worker_finished_ &&
         (frames_.size() >= kFrameCapacity ||
          (waiting_for_input_ && packets_.empty() && !input_finished_));
}

std::size_t FfmpegFrameDecoder::queued_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return queued_bytes_;
}

bool FfmpegFrameDecoder::waiting_for_packet() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && !worker_finished_ && waiting_for_input_ &&
         packets_.empty() && frames_.empty() && !input_finished_;
}

void FfmpegFrameDecoder::ReleasePacket(std::size_t bytes,
                                       bool queued) noexcept {
  if (!bytes) return;
  if (queued) {
    std::lock_guard<std::mutex> lock(mutex_);
    queued_bytes_ -= bytes;
  }
  try {
    release_(bytes);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "解码器 Packet 释放回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "解码器 Packet 释放回调抛出了未知异常");
  }
}

void FfmpegFrameDecoder::NotifyChanged() noexcept {
  try {
    notify_();
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "解码器通知回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "解码器通知回调抛出了未知异常");
  }
}

void FfmpegFrameDecoder::Run() noexcept {
  try {
    bool drain_sent = false;
    std::int64_t next_pts = AV_NOPTS_VALUE;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) break;
      }
      ffmpeg::Frame frame;
      const auto result = decoder_->ReceiveFrame(frame);
      if (result == ffmpeg::DecodeResult::kFrame) {
        // Interlaced frames wider than 1024 pixels use top-field-first order
        // regardless of codec.
        if (frame->width > 1024 && (frame->flags & AV_FRAME_FLAG_INTERLACED)) {
          frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
        }
        frame->pts = frame->best_effort_timestamp;
        next_pts = decoder_->FrameEndPts(
            frame, parser_repeat_pict_ ? parser_repeat_pict_() : -1);
        {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait(lock, [this] {
            return stopping_ || frames_.size() < kFrameCapacity;
          });
          if (stopping_) break;
          frames_.push_back(std::move(frame));
        }
        NotifyChanged();
        continue;
      }
      if (result == ffmpeg::DecodeResult::kEnd) {
        decoder_->Flush();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) end_pts_ = next_pts;
        break;
      }
      ffmpeg::Packet packet;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        waiting_for_input_ = true;
        changed_.wait(lock, [this] {
          return stopping_ || !packets_.empty() || input_finished_;
        });
        waiting_for_input_ = false;
        if (stopping_) break;
        if (!packets_.empty()) {
          packet = std::move(packets_.front());
          packets_.pop_front();
        }
      }
      if (packet.get() && packet->size > 0) {
        const auto bytes = static_cast<std::size_t>(packet->size);
        try {
          if (!decoder_->SendPacket(packet)) {
            throw ffmpeg::FfmpegException(AVERROR_BUG,
                                          "解码器同时拒绝输入和输出");
          }
        } catch (...) {
          ReleasePacket(bytes);
          throw;
        }
        ReleasePacket(bytes);
        NotifyChanged();
      } else if (!drain_sent) {
        if (!decoder_->Drain()) {
          throw ffmpeg::FfmpegException(AVERROR_BUG, "解码器拒绝 EOF 输入");
        }
        drain_sent = true;
      } else {
        throw ffmpeg::FfmpegException(AVERROR_BUG, "解码器在 EOF 后仍需要输入");
      }
    }
  } catch (...) {
    std::deque<ffmpeg::Packet> discarded;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = std::current_exception();
      worker_finished_ = true;
      discarded.swap(packets_);
    }
    // A failed codec cannot consume these reservations. Release them before
    // the caller drains output, so another track can still receive its packets.
    // Stop swaps the same queue under the lock and cannot release them twice.
    for (const auto& packet : discarded) {
      ReleasePacket(static_cast<std::size_t>(packet->size));
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_finished_ = true;
  }
  NotifyChanged();
}

}  // namespace mw::streamer::internal
