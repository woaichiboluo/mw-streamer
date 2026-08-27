from __future__ import annotations

import hashlib
import time
from pathlib import Path

import pytest

from mw_e2e.config import ConfigurationError, discover_media
from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe, MediaPublisher
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, MediaAsset
from mw_e2e.runner import Runner
from mw_e2e.sync import find_recording


_STABILITY_SECONDS = 60.0


@pytest.fixture(scope="module")
def dual_sink_media_assets(pytestconfig: pytest.Config) -> dict[str, MediaAsset]:
    try:
        assets = discover_media(pytestconfig.getoption("--e2e-config"))
    except ConfigurationError as error:
        raise pytest.UsageError(str(error)) from error
    selected: dict[str, MediaAsset] = {}
    for codec in ("h264", "hevc"):
        candidates = [asset for asset in assets if asset.video_codec == codec]
        if not candidates:
            raise pytest.UsageError(f"双Sink测试缺少{codec}真实视频")
        selected[codec] = min(
            candidates,
            key=lambda asset: (
                (asset.video_width or 0) * (asset.video_height or 0),
                asset.identifier,
            ),
        )
    return selected


def _assert_local_sink(
    events: list[dict[str, str]], asset: MediaAsset
) -> None:
    summaries = [event for event in events if event.get("event") == "summary"]
    assert summaries, "runner缺少summary事件"
    summary = summaries[-1]
    assert summary.get("local_sink_starts") == "1"
    assert summary.get("local_sink_stops") == "1"
    assert int(summary.get("local_sink_video_frames", "0")) > 0
    if asset.has_audio:
        assert int(summary.get("local_sink_audio_frames", "0")) > 0
        assert int(summary.get("audio_encode_samples", "0")) > 0
    else:
        assert summary.get("local_sink_audio_frames") == "0"
        assert summary.get("audio_encode_samples") == "0"
    assert summary.get("local_sink_invalid_frames") == "0"
    assert int(summary.get("video_encode_frames", "0")) > 0


@pytest.mark.smoke
@pytest.mark.parametrize("video_codec", ["h264", "hevc"])
@pytest.mark.parametrize("output_protocol", ["rtmp", "rtsp", "file", "srt"])
def test_pull_to_local_and_encoded_sink(
    output_protocol: str,
    video_codec: str,
    dual_sink_media_assets: dict[str, MediaAsset],
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    media_asset = dual_sink_media_assets[video_codec]
    digest = hashlib.sha1(
        str(artifact_directory).encode(), usedforsecurity=False
    ).hexdigest()[:12]
    source_path = f"dual-sink/source-{output_protocol}-{digest}"
    publisher = MediaPublisher(
        e2e_config,
        media_asset,
        media_environment.source.publish_url("srt", source_path)
        + "&pkt_size=1316",
        artifact_directory / "publisher",
    )
    publisher.start()
    media_environment.source.wait_for_path(
        source_path, settings.startup_timeout_seconds, publisher.process
    )

    output_path = f"dual-sink/output-{output_protocol}-{digest}"
    requested_recording = artifact_directory / "encoded-output.mp4"
    sink = None
    if output_protocol == "file":
        output_url = str(requested_recording)
    else:
        sink = media_environment.sinks[output_protocol]
        output_url = sink.publish_url(output_protocol, output_path)

    runner = Runner(
        e2e_config,
        runner_path,
        media_asset,
        media_environment.source.read_url("srt", source_path),
        [output_url],
        settings.startup_timeout_seconds * 2 + _STABILITY_SECONDS,
        artifact_directory,
        local_sink=True,
    )
    probe = None
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "local_sink_started",
            settings.startup_timeout_seconds,
        )
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        if sink is not None:
            sink.wait_for_path(
                output_path, settings.startup_timeout_seconds, runner.process
            )
            probe = MediaProbe(
                e2e_config,
                "rtsp",
                sink.read_url("rtsp", output_path),
                media_asset,
                _STABILITY_SECONDS,
                artifact_directory,
                f"probe-{output_protocol}",
            )
            probe.start()
            probe.wait(
                settings.startup_timeout_seconds + _STABILITY_SECONDS
            )
        else:
            deadline = time.monotonic() + _STABILITY_SECONDS
            while time.monotonic() < deadline:
                runner.process.ensure_running()
                time.sleep(0.1)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()
        publisher.stop()

    if output_protocol == "file":
        recording = find_recording(requested_recording)
        probe = MediaProbe(
            e2e_config,
            "file",
            str(recording),
            media_asset,
            _STABILITY_SECONDS,
            artifact_directory,
            "probe-file",
        )
        try:
            probe.start()
            probe.wait(settings.startup_timeout_seconds + _STABILITY_SECONDS)
        finally:
            probe.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    _assert_local_sink(events, media_asset)
