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
        scenario: Literal["streaming", "remux", "rtsp_publish", "file"] = "streaming",
        input_output_urls: Sequence[str] = (),
        rtsp_publish_paths: Sequence[str] = (),
        rtsp_port: int | None = None,
        passthrough_video: bool = False,
        software_video: bool = False,
        local_sink: bool = False,
        observe_cache: bool = False,
        video_jitter_ms: tuple[int, int] | None = None,
    ) -> None:
        if scenario not in {"streaming", "remux", "rtsp_publish", "file"}:
            raise ValueError(f"未知测试场景: {scenario}")
        if scenario == "remux" and not output_urls:
            raise ValueError("Remux 场景 至少需要一个输出目标")
        if scenario == "remux" and input_output_urls:
            raise ValueError("Remux 场景请通过output_urls配置输出")
        if scenario == "file" and (output_urls or input_output_urls):
            raise ValueError("文件分析场景不支持输出目标")
        if scenario == "rtsp_publish":
            if output_urls or input_output_urls:
                raise ValueError("RTSP发布场景不支持其他输出目标")
            if not rtsp_publish_paths:
                raise ValueError("RTSP发布场景需要发布路径")
        if bool(rtsp_publish_paths) != (rtsp_port is not None):
            raise ValueError("RTSP发布路径和端口必须一起提供")
        if scenario not in {"rtsp_publish", "streaming"} and rtsp_publish_paths:
            raise ValueError("只有RTSP发布和Streaming场景支持发布路径")
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
        if rtsp_port is not None:
            command.extend(["--rtsp-port", str(rtsp_port)])
        for path in rtsp_publish_paths:
            command.extend(["--rtsp-publish", path])
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
