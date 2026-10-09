#ifndef ZLMEDIAKIT_REMUXRECORDER_H
#define ZLMEDIAKIT_REMUXRECORDER_H

#include "Common/MediaSink.h"

namespace mediakit {

// Per-output recording uses one common audio/video origin, in milliseconds.
// All operations run on the owning muxer's poller.
class RemuxRecorder : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<RemuxRecorder>;
    void setTimestampOrigin(uint64_t stamp);
    bool inputFrame(const Frame::Ptr &frame) override;
    virtual void close() = 0;

protected:
    virtual bool inputFrame_l(const Frame::Ptr &frame) = 0;

private:
    bool _have_origin = false;
    bool _started = false;
    uint64_t _origin = 0;
};

RemuxRecorder::Ptr createRemuxRecorder(const std::string &file_path, bool hls_fmp4);

} // namespace mediakit
#endif
