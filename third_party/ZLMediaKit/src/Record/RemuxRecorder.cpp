#include "RemuxRecorder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include "mw/log.h"
#include "MP4Muxer.h"
#include "HlsMakerImp.h"

namespace mediakit {
namespace {

class FileMuxer final : public MP4MuxerInterface {
public:
    explicit FileMuxer(const std::string &path) {
        setPreservePackets(true);
        _file = std::make_shared<MP4FileDisk>();
        _file->openFile(path.c_str(), "wb+");
    }
    void close() {
        flush();
        MP4MuxerInterface::resetTracks();
        _file->closeFileChecked();
    }
protected:
    MP4FileIO::Writer createWriter() override {
        return _file->createWriter(MOV_FLAG_PRESERVE_TIMESTAMPS, true);
    }
private:
    MP4FileDisk::Ptr _file;
};

class HlsMuxer final : public MP4MuxerMemory {
public:
    explicit HlsMuxer(const std::string &path) {
        setPreservePackets(true);
        auto directory = path.substr(0, path.size() - 5);
        _hls = std::make_shared<HlsMakerImp>(true, path, "", 64 * 1024,
                                            10.0f, 0u, true, ".mp4", directory, true);
    }
    void addTrackCompleted() override {
        const auto &data = getInitSegment();
        _hls->inputInitSegment(data.data(), data.size());
    }
    void close() {
        flush();
        MP4MuxerMemory::resetTracks();
        _hls->finish();
    }
protected:
    void onSegmentData(std::string data, uint64_t stamp, bool key) override {
        if (!data.empty()) {
            // AAC priming may precede the first video keyframe. Open the first
            // segment for that fragment rather than silently discarding it.
            _hls->inputData(data.data(), data.size(), stamp, key || !_segment_started);
            _segment_started = true;
        }
    }
private:
    std::shared_ptr<HlsMakerImp> _hls;
    bool _segment_started = false;
};

template <typename Muxer>
class RecorderImp final : public RemuxRecorder {
public:
    explicit RecorderImp(const std::string &path) : _muxer(path) {}
    ~RecorderImp() override {
        try {
            close();
        } catch (const std::exception &ex) {
            MW_LOG_WARNING("zlm", "{}", ex.what());
        }
    }
    bool addTrack(const Track::Ptr &track) override {
        if (_closed) throw std::logic_error("Recording already closed");
        return _muxer.addTrack(track);
    }
    void addTrackCompleted() override {
        if (_closed) throw std::logic_error("Recording already closed");
        _muxer.addTrackCompleted();
    }
    void flush() override {
        if (!_closed) _muxer.flush();
    }
    void close() override {
        if (!_closed) {
            _muxer.close();
            _closed = true;
        }
    }
    void resetTracks() override { close(); }
protected:
    bool inputFrame_l(const Frame::Ptr &frame) override {
        if (_closed) throw std::logic_error("Recording already closed");
        return _muxer.inputFrame(frame);
    }
private:
    Muxer _muxer;
    bool _closed = false;
};
} // namespace

void RemuxRecorder::setTimestampOrigin(uint64_t stamp) {
    if (_started) throw std::logic_error("Recording timestamp origin already in use");
    _origin = stamp;
    _have_origin = true;
}

bool RemuxRecorder::inputFrame(const Frame::Ptr &frame) {
    if (!_have_origin) {
        setTimestampOrigin(std::min(frame->dts(), frame->pts()));
    }
    if (frame->dts() < _origin || frame->pts() < _origin ||
        frame->dts() - _origin > uint64_t(std::numeric_limits<int64_t>::max()) ||
        frame->pts() - _origin > uint64_t(std::numeric_limits<int64_t>::max())) {
        throw std::invalid_argument("Recording timestamp precedes the common origin");
    }
    auto shifted = std::make_shared<FrameStamp>(frame);
    shifted->setStamp(int64_t(frame->dts() - _origin), int64_t(frame->pts() - _origin));
    _started = true;
    return inputFrame_l(shifted);
}

RemuxRecorder::Ptr createRemuxRecorder(const std::string &file_path, bool hls_fmp4) {
    auto path = file_path;
    std::replace(path.begin(), path.end(), '\\', '/');
    auto extension = hls_fmp4 ? ".m3u8" : ".mp4";
    if (path.size() < std::char_traits<char>::length(extension) ||
        path.compare(path.size() - std::char_traits<char>::length(extension),
                     std::char_traits<char>::length(extension), extension) != 0) {
        throw std::invalid_argument("Invalid recording file extension");
    }
    if (hls_fmp4) return std::make_shared<RecorderImp<HlsMuxer>>(path);
    return std::make_shared<RecorderImp<FileMuxer>>(path);
}

} // namespace mediakit
