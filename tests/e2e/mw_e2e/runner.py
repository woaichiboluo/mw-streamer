from __future__ import annotations

from pathlib import Path
from typing import Literal, Sequence

from .models import E2EConfig, MediaAsset
from .process import ManagedProcess, ProcessError


class Runner:
    """Launch one Pipeline with the sink graph selected by the test scenario."""

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
        *,
        scenario: Literal["streaming", "remux", "file"] = "streaming",
        input_output_urls: Sequence[str] = (),
        passthrough_video: bool = False,
        software_video: bool = False,
        local_sink: bool = False,
        observe_cache: bool = False,
        video_jitter_ms: tuple[int, int] | None = None,
    ) -> None:
        if scenario not in {"streaming", "remux", "file"}:
            raise ValueError(f"未知测试场景: {scenario}")
        if scenario == "remux" and not output_urls:
            raise ValueError("Remux 场景 至少需要一个输出目标")
        if scenario == "remux" and input_output_urls:
            raise ValueError("Remux 场景请通过output_urls配置输出")
        if scenario == "file" and (output_urls or input_output_urls):
            raise ValueError("文件分析场景不支持输出目标")
        if observe_cache and scenario != "streaming":
            raise ValueError("缓存观测仅支持 streaming 场景")
        self.events_path = artifact_directory / "runner.events"
        command = [
            str(executable),
            "--scenario",
            scenario,
            "--input",
            input_url,
            "--events",
            str(self.events_path),
            "--duration-ms",
            str(int(duration_seconds * 1000)),
        ]
        if scenario == "streaming":
            effective_cache_duration_ms = (
                config.tests.cache_duration_ms
                if cache_duration_ms is None
                else cache_duration_ms
            )
            command.extend(
                [
                    "--cache-ms",
                    str(effective_cache_duration_ms),
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
            )
            if passthrough_video:
                command.append("--passthrough-video")
            if software_video:
                command.append("--software-video")
            if local_sink:
                command.append("--local-sink")
            if observe_cache:
                command.append("--observe-cache")
            if video_jitter_ms is not None:
                minimum, maximum = video_jitter_ms
                if minimum < 0 or maximum < minimum:
                    raise ValueError("视频抖动范围无效")
                command.extend(
                    [
                        "--video-jitter-min-ms",
                        str(minimum),
                        "--video-jitter-max-ms",
                        str(maximum),
                    ]
                )
            for input_output_url in input_output_urls:
                command.extend(["--input-output", input_output_url])
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
