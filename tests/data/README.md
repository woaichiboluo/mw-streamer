#编解码测试媒体

目录中的 MP4 文件只包含由
        FFmpeg `testsrc2` 生成的 160×144、5 fps、5 帧合成视频， 分别使用
            H .264 和 H .265 编码。160×144 同时满足当前测试显卡上两个 CUVID
                解码器的
                    最小分辨率要求。

`h264_aac_mono_44100.mp4` 在 H .264 样本上增加了 44.1 kHz 单声道 AAC，用于验证
                        Demuxer 的音视频 packet 路由和 AudioAutoDecoder
                            自动选择、解码及 drain。

`h264_aac_mono_44100_1s.mp4` 保留完整的 1 秒、5 帧
                                H .264 视频，并增加 1 秒 44.1 kHz 单声道
                                    AAC。该文件专为单路 FILE Pipeline
                                        端到端测试保留， 以便同时验证连续视频
                                            PTS、音频归一化和完整 EOF drain。

                                                生成命令：

```bash ffmpeg -
    hide_banner - loglevel error - y - fflags + bitexact - f lavfi -
    i testsrc2 = size = 160x144
    : rate = 5 - frames : v 5 - map_metadata - 1 - an -
                          c : v libx264 - preset ultrafast - tune zerolatency -
                              pix_fmt yuv420p - g 5 - bf 0 -
                              flags : v + bitexact - movflags +
                                      faststart h264_160x144.mp4

                                          ffmpeg -
                                      hide_banner - loglevel error - y -
                                      fflags + bitexact - f lavfi - i testsrc2 =
                 size = 160x144
    : rate = 5 - frames : v 5 - map_metadata - 1 - an -
                          c : v libx265 - preset ultrafast - pix_fmt yuv420p -
                              g 5 - bf 0 - threads 1 -
                              flags
    : v +
      bitexact - x265 -
      params 'log-level=error:pools=1:frame-threads=1:keyint=5:min-keyint=5:scenecut=0' -
      tag : v hvc1 - movflags +
            faststart h265_160x144.mp4

                ffmpeg -
            hide_banner - loglevel error - y - i h264_160x144.mp4 - f lavfi -
            i sine = frequency = 1000 : sample_rate = 44100
    : duration = 0.2 - map 0 : v : 0 - map 1 : a : 0 - c : v copy -
                                                           c
    : a aac -
      ac 1 -
      shortest h264_aac_mono_44100.mp4

          ffmpeg -
      hide_banner - loglevel error - y - i h264_160x144.mp4 - f lavfi - i sine =
                     frequency = 1000 : sample_rate = 44100
    : duration = 1 - map 0 : v : 0 - map 1 : a : 0 - c : v copy -
                                                         c
    : a aac -
      ac 1 - shortest h264_aac_mono_44100_1s.mp4
```

`h264_1s_aac_3s.mp4` 和对应的 MPEG-TS/HLS VOD 清单保留 1 秒视频与约 3 秒音频，
用于验证有限输入在最终视频时间线结束后丢弃多余音频并正常完成：

```bash
ffmpeg -hide_banner -loglevel error -fflags +bitexact \
  -i h264_160x144.mp4 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100:duration=3 \
  -map 0:v:0 -map 1:a:0 -map_metadata -1 \
  -c:v copy -c:a aac -ac 1 -flags:a +bitexact \
  h264_1s_aac_3s.mp4

ffmpeg -hide_banner -loglevel error -fflags +bitexact \
  -i h264_1s_aac_3s.mp4 -map 0:v:0 -map 0:a:0 \
  -c copy -f mpegts h264_1s_aac_3s.ts
```
