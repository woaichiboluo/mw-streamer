from __future__ import annotations

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
        command.extend(
            [
                "-c",
                "copy",
                "-f",
                "mpegts",
                output_url,
            ]
        )
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
        duration_seconds: float,
        artifact_directory: Path,
        name: str,
    ) -> None:
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
        command.extend(["-i", input_url, "-t", str(duration_seconds)])
        if asset.has_video:
            command.extend(["-map", "0:v:0"])
        if asset.has_audio:
            command.extend(["-map", "0:a:0"])
        command.extend(["-f", "null", "-"])
        self.process = ManagedProcess(command, artifact_directory / f"{name}.log")
        self.duration_seconds = duration_seconds

    def start(self) -> None:
        self.process.start()

    def wait(self, timeout: float) -> None:
        returncode = self.process.wait(timeout)
        if returncode != 0:
            raise ProcessError(
                f"FFmpeg 接收探针失败，返回码 {returncode}: "
                f"{self.process.log_path}"
            )
        progress = self._read_progress()
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

    def _read_progress(self) -> dict[str, str]:
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
