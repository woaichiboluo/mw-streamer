/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef DEVICE_DEVICE_H_
#define DEVICE_DEVICE_H_

#include <memory>
#include <string>
#include <functional>
#include "Util/TimeTicker.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class VideoInfo {
public:
    CodecId codecId = CodecH264;
    int iWidth;
    int iHeight;
    float iFrameRate;
    int iBitRate = 2 * 1024 * 1024;
};

class AudioInfo {
public:
    CodecId codecId = CodecAAC;
    int iChannel;
    int iSampleBit;
    int iSampleRate;
};

/**
 * MultiMediaSourceMuxer类的包装，方便初学者使用
 * Wrapper class for MultiMediaSourceMuxer, making it easier for beginners to use.
 
 * [AUTO-TRANSLATED:101887bd]
 */
class DevChannel  : public MultiMediaSourceMuxer{
public:
    using Ptr = std::shared_ptr<DevChannel>;

    // fDuration<=0为直播，否则为点播  [AUTO-TRANSLATED:e3b6029a]
    // fDuration<=0 for live streaming, otherwise for on-demand
    DevChannel(const MediaTuple& tuple, float duration = 0, const ProtocolOption &option = ProtocolOption())
        : MultiMediaSourceMuxer(tuple, duration, option) {}

    /**
     * 初始化视频Track
     * 相当于MultiMediaSourceMuxer::addTrack(VideoTrack::Ptr );
     * @param info 视频相关信息
     * Initialize the video Track
     * Equivalent to MultiMediaSourceMuxer::addTrack(VideoTrack::Ptr );
     * @param info Video related information
     
     * [AUTO-TRANSLATED:6845d52d]
     */
    bool initVideo(const VideoInfo &info);

    /**
     * 初始化音频Track
     * 相当于MultiMediaSourceMuxer::addTrack(AudioTrack::Ptr );
     * @param info 音频相关信息
     * Initialize the audio Track
     * Equivalent to MultiMediaSourceMuxer::addTrack(AudioTrack::Ptr );
     * @param info Audio related information
     
     * [AUTO-TRANSLATED:5be9d272]
     */
    bool initAudio(const AudioInfo &info);

    /**
     * 输入264帧
     * @param data 264单帧数据指针
     * @param len 数据指针长度
     * @param dts 解码时间戳，单位毫秒；等于0时内部会自动生成时间戳
     * @param pts 播放时间戳，单位毫秒；等于0时内部会赋值为dts
     * Input 264 frame
     * @param data 264 single frame data pointer
     * @param len Data pointer length
     * @param dts Decode timestamp, in milliseconds; If it is 0, the timestamp will be generated automatically internally
     * @param pts Play timestamp, in milliseconds; If it is 0, it will be assigned to dts internally
     
     * [AUTO-TRANSLATED:bda112e9]
     */
    bool inputH264(const char *data, int len, uint64_t dts, uint64_t pts = 0);

    /**
     * 输入265帧
     * @param data 265单帧数据指针
     * @param len 数据指针长度
     * @param dts 解码时间戳，单位毫秒；等于0时内部会自动生成时间戳
     * @param pts 播放时间戳，单位毫秒；等于0时内部会赋值为dts
     * Input 265 frame
     * @param data 265 single frame data pointer
     * @param len Data pointer length
     * @param dts Decode timestamp, in milliseconds; If it is 0, the timestamp will be generated automatically internally
     * @param pts Play timestamp, in milliseconds; If it is 0, it will be assigned to dts internally
     
     * [AUTO-TRANSLATED:1fc1c892]
     */
    bool inputH265(const char *data, int len, uint64_t dts, uint64_t pts = 0);

    /**
     * 输入aac帧
     * @param data_without_adts 不带adts头的aac帧
     * @param len 帧数据长度
     * @param dts 时间戳，单位毫秒
     * @param adts_header adts头
     * Input aac frame
     * @param data_without_adts aac frame without adts header
     * @param len Frame data length
     * @param dts Timestamp, in milliseconds
     * @param adts_header adts header
     
     * [AUTO-TRANSLATED:6eca0279]
     */
    bool inputAAC(const char *data_without_adts, int len, uint64_t dts, const char *adts_header);

    /**
     * 输入OPUS/G711音频帧
     * @param data 音频帧
     * @param len 帧数据长度
     * @param dts 时间戳，单位毫秒
     * Input OPUS/G711 audio frame
     * @param data Audio frame
     * @param len Frame data length
     * @param dts Timestamp, in milliseconds
     
     * [AUTO-TRANSLATED:5f13cdf6]
     */
    bool inputAudio(const char *data, int len, uint64_t dts);

    // // 重载基类方法，确保线程安全 ////  [AUTO-TRANSLATED:86e2df12]
    // // Override base class methods to ensure thread safety ////
    bool inputFrame(const Frame::Ptr &frame) override;
    bool addTrack(const Track::Ptr & track) override;
    void addTrackCompleted() override;

private:
    MediaOriginType getOriginType(MediaSource &sender) const override;

private:
    std::shared_ptr<VideoInfo> _video;
    std::shared_ptr<AudioInfo> _audio;
    toolkit::SmoothTicker _aTicker[2];
};

} /* namespace mediakit */

#endif /* DEVICE_DEVICE_H_ */
