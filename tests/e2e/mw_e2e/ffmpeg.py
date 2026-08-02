from __future__ import annotations

import math
import time
from pathlib import Path

from .models import E2EConfig, MediaAsset
from .process import ManagedProcess, ProcessError


class MediaPublisher:
    def __init__(
        self,
        config: E2EConfig,
        asset: MediaAsset,
        output_url: str,
        artifact_directory: Path,
    ) -> None:
        command = [
            str(config.tools.ffmpeg),
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "info",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            str(asset.path),
        ]
        if asset.has_video:
            command.extend(["-map", "0:v:0"])
        if asset.has_audio:
            command.extend(["-map", "0:a:0"])
        command.extend(["-c", "copy"])
        if output_url.startswith("srt://"):
            command.extend(["-f", "mpegts"])
        elif output_url.startswith("rtmp://"):
            command.extend(["-f", "flv"])
        elif output_url.startswith("rtsp://"):
            command.extend(
                [
                    "-copyts",
                    "-start_at_zero",
                    "-avoid_negative_ts",
                    "disabled",
                    "-rtsp_transport",
                    "tcp",
                    "-muxdelay",
                    "0",
                    "-f",
                    "rtsp",
                ]
            )
        else:
            raise ValueError(f"媒体发布地址协议不受支持: {output_url}")
        command.append(output_url)
        self.process = ManagedProcess(
            command, artifact_directory / "publisher.log"
        )

    def start(self) -> None:
        self.process.start()

    def stop(self) -> None:
        self.process.stop()


class MediaProbe:
    def __init__(
        self,
        config: E2EConfig,
        protocol: str,
        input_url: str,
        asset: MediaAsset,
        duration_seconds: float | None,
        artifact_directory: Path,
        name: str,
        *,
        listen: bool = False,
        listen_timeout_seconds: float | None = None,
        stream_copy: bool = False,
    ) -> None:
        if listen and protocol != "rtsp":
            raise ValueError("只有RTSP探针支持监听模式")
        if listen and (
            listen_timeout_seconds is None or listen_timeout_seconds <= 0
        ):
            raise ValueError("RTSP监听探针必须提供正数超时")

        self.progress_path = artifact_directory / f"{name}.progress"
        command = [
            str(config.tools.ffmpeg),
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "info",
            "-progress",
            str(self.progress_path),
            "-stats_period",
            "1",
        ]
        if protocol == "rtsp":
            command.extend(["-rtsp_transport", "tcp"])
        if listen:
            command.extend(
                [
                    "-rtsp_flags",
                    "listen",
                    "-listen_timeout",
                    str(math.ceil(listen_timeout_seconds)),
                ]
            )
        command.extend(["-i", input_url])
        if duration_seconds is not None:
            command.extend(["-t", str(duration_seconds)])
        if asset.has_video:
            command.extend(["-map", "0:v:0"])
        if asset.has_audio:
            command.extend(["-map", "0:a:0"])
        if stream_copy:
            command.extend(["-c", "copy"])
        command.extend(["-f", "null", "-"])
        self.process = ManagedProcess(command, artifact_directory / f"{name}.log")
        self.duration_seconds = duration_seconds

    def start(self) -> None:
        self.process.start()

    def wait(self, timeout: float) -> None:
        if self.duration_seconds is None:
            raise ValueError("无固定时长的媒体探针不能调用wait")
        returncode = self.process.wait(timeout)
        if returncode != 0:
            raise ProcessError(
                f"FFmpeg 接收探针失败，返回码 {returncode}: "
                f"{self.process.log_path}"
            )
        progress = self.read_progress()
        if progress.get("progress") != "end":
            raise ProcessError(f"FFmpeg 接收探针没有正常结束: {self.progress_path}")
        output_time_us = int(
            progress.get("out_time_us", progress.get("out_time_ms", "0"))
        )
        minimum_time_us = int(max(0.0, self.duration_seconds - 1.0) * 1_000_000)
        if output_time_us < minimum_time_us:
            raise ProcessError(
                f"FFmpeg 接收媒体时长不足: {output_time_us}us < "
                f"{minimum_time_us}us"
            )

    def stop(self) -> None:
        self.process.stop()

    def read_progress(self) -> dict[str, str]:
        values: dict[str, str] = {}
        try:
            lines = self.progress_path.read_text(encoding="utf-8").splitlines()
        except OSError as error:
            raise ProcessError(f"无法读取 FFmpeg progress: {error}") from error
        for line in lines:
            key, separator, value = line.partition("=")
            if separator:
                values[key] = value
        return values


class MediaRecorder:
    def __init__(
        self,
        config: E2EConfig,
        protocol: str,
        input_url: str,
        asset: MediaAsset,
        duration_seconds: float,
        output_path: Path,
        artifact_directory: Path,
        *,
        listen: bool = False,
        listen_timeout_seconds: float | None = None,
    ) -> None:
        if duration_seconds <= 0:
            raise ValueError("媒体录制时长必须大于0")
        if listen and protocol != "rtsp":
            raise ValueError("只有RTSP录像支持监听模式")
        if listen and (
            listen_timeout_seconds is None or listen_timeout_seconds <= 0
        ):
            raise ValueError("RTSP监听录像必须提供正数超时")
        command = [
            str(config.tools.ffmpeg),
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "info",
        ]
        if protocol == "rtsp":
            command.extend(["-rtsp_transport", "tcp"])
        if listen:
            command.extend(
                [
                    "-rtsp_flags",
                    "listen",
                    "-listen_timeout",
                    str(math.ceil(listen_timeout_seconds)),
                ]
            )
        command.extend(["-i", input_url, "-t", str(duration_seconds)])
        if asset.has_video:
            command.extend(["-map", "0:v:0"])
        if asset.has_audio:
            command.extend(["-map", "0:a:0"])
        if asset.has_video:
            command.extend(["-c:v", "copy"])
        if asset.has_audio:
            command.extend(["-c:a", "pcm_s16le"])
        command.extend(["-y", str(output_path)])
        self.output_path = output_path
        self.process = ManagedProcess(
            command, artifact_directory / "recorder.log"
        )

    def start(self) -> None:
        self.process.start()

    def wait(self, timeout: float) -> None:
        returncode = self.process.wait(timeout)
        if returncode != 0:
            raise ProcessError(
                f"FFmpeg媒体录制失败，返回码{returncode}: "
                f"{self.process.log_path}"
            )
        if not self.output_path.is_file():
            raise ProcessError(f"FFmpeg没有生成录制文件: {self.output_path}")

    def stop(self) -> None:
        self.process.stop()


def monitor_stable_probes(
    probes: list[MediaProbe],
    duration_seconds: float,
    stall_timeout_seconds: float,
) -> None:
    if not probes:
        raise ValueError("至少需要一个媒体探针")
    if duration_seconds <= 0 or stall_timeout_seconds <= 0:
        raise ValueError("探针超时必须大于0")

    last_media_time = {id(probe): -1 for probe in probes}
    last_frame = {id(probe): -1 for probe in probes}
    last_change_at = {id(probe): time.monotonic() for probe in probes}
    first_media_time: dict[int, int] = {}
    monitor_deadline: float | None = None

    while monitor_deadline is None or time.monotonic() < monitor_deadline:
        now = time.monotonic()
        for probe in probes:
            probe_id = id(probe)
            if probe.process.returncode is not None:
                raise ProcessError(
                    f"Bench媒体探针提前退出，返回码"
                    f"{probe.process.returncode}: {probe.process.log_path}"
                )
            if not probe.progress_path.exists():
                if now - last_change_at[probe_id] > stall_timeout_seconds:
                    raise ProcessError(
                        f"FFmpeg接收探针启动后没有媒体进度: "
                        f"{probe.process.log_path}"
                    )
                continue

            progress = probe.read_progress()
            media_time = int(
                progress.get(
                    "out_time_us", progress.get("out_time_ms", "0")
                )
            )
            frame = int(progress.get("frame", "0"))
            if (
                media_time > last_media_time[probe_id]
                or frame > last_frame[probe_id]
            ):
                first_media_time.setdefault(probe_id, media_time)
                last_media_time[probe_id] = media_time
                last_frame[probe_id] = frame
                last_change_at[probe_id] = now
            elif now - last_change_at[probe_id] > stall_timeout_seconds:
                raise ProcessError(
                    f"FFmpeg接收探针媒体进度停顿超过"
                    f"{stall_timeout_seconds:g}秒: {probe.process.log_path}"
                )

        if monitor_deadline is None and len(first_media_time) == len(probes):
            monitor_deadline = now + duration_seconds
        time.sleep(1.0)

    minimum_media_duration_us = int(duration_seconds * 0.98 * 1_000_000)
    for probe in probes:
        probe_id = id(probe)
        media_duration_us = (
            last_media_time[probe_id] - first_media_time[probe_id]
        )
        if media_duration_us < minimum_media_duration_us:
            raise ProcessError(
                f"Bench接收媒体时长不足: {media_duration_us}us < "
                f"{minimum_media_duration_us}us: {probe.process.log_path}"
            )
