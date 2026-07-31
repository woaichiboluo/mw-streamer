from __future__ import annotations

from pathlib import Path
from typing import Sequence

from .models import E2EConfig, MediaAsset
from .process import ManagedProcess, ProcessError


class Runner:
    def __init__(
        self,
        config: E2EConfig,
        executable: Path,
        asset: MediaAsset,
        input_url: str,
        output_urls: Sequence[str],
        duration_seconds: float,
        artifact_directory: Path,
        cache_duration_ms: int | None = None,
    ) -> None:
        if not output_urls:
            raise ValueError("StreamingPipeline E2E 至少需要一个输出目标")
        self.events_path = artifact_directory / "runner.events"
        effective_cache_duration_ms = (
            config.tests.cache_duration_ms
            if cache_duration_ms is None
            else cache_duration_ms
        )
        command = [
            str(executable),
            "--input",
            input_url,
            "--events",
            str(self.events_path),
            "--cache-ms",
            str(effective_cache_duration_ms),
            "--duration-ms",
            str(int(duration_seconds * 1000)),
            "--output-width",
            str(asset.video_width or 0),
            "--output-height",
            str(asset.video_height or 0),
            "--frame-rate-num",
            str(asset.video_frame_rate_num or 0),
            "--frame-rate-den",
            str(asset.video_frame_rate_den or 1),
            "--video-codec",
            (
                "h265"
                if asset.video_codec == "hevc"
                else (asset.video_codec or "none")
            ),
        ]
        for output_url in output_urls:
            command.extend(["--output", output_url])
        self.process = ManagedProcess(
            command, artifact_directory / "runner.log"
        )

    def start(self) -> None:
        self.process.start()

    def interrupt_and_wait(self, timeout: float) -> None:
        self.process.interrupt()
        self.wait(timeout)

    def wait(self, timeout: float) -> None:
        returncode = self.process.wait(timeout)
        if returncode != 0:
            raise ProcessError(
                f"E2E runner 失败，返回码 {returncode}: "
                f"{self.process.log_path}"
            )

    def stop(self) -> None:
        self.process.stop()
