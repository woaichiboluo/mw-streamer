from __future__ import annotations

import socket
import time
from pathlib import Path

import pytest

from mw_e2e.config import probe_media_file
from mw_e2e.events import (
    assert_pipeline_succeeded,
    final_performance,
    read_events,
    wait_for_event,
)
from mw_e2e.ffmpeg import MediaProbe, MediaPublisher, MediaRecorder
from mw_e2e.mediamtx import MediaEnvironment, allocate_tcp_port
from mw_e2e.models import E2EConfig, MediaAsset
from mw_e2e.runner import Runner
from mw_e2e.sync import analyze_sync


_SYNC_RECORD_SECONDS = 45.0
_SYNC_IGNORE_BEFORE_SECONDS = 15.0


def _can_connect(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        return False


@pytest.mark.smoke
def test_rtsp_publish_starts_on_input_and_serves_two_paths(
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    sample_path = Path(__file__).parents[1] / "data" / "h264_aac.mp4"
    asset = probe_media_file(e2e_config, sample_path)
    port = allocate_tcp_port()
    source_path = "live/rtsp-publish-source"
    runner = Runner(
        e2e_config,
        runner_path,
        asset,
        media_environment.source.read_url("srt", source_path),
        [],
        settings.startup_timeout_seconds * 3 + 5.0,
        artifact_directory,
        scenario="rtsp_publish",
        rtsp_publish_paths=["live/first", "live/second"],
        rtsp_port=port,
    )
    publisher = MediaPublisher(
        e2e_config,
        asset,
        media_environment.source.publish_url("srt", source_path)
        + "&pkt_size=1316",
        artifact_directory,
    )
    probes: list[MediaProbe] = []
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "runner_started",
            settings.startup_timeout_seconds,
        )
        assert not _can_connect(port), "收到输入轨道前不应启动RTSP监听"

        publisher.start()
        media_environment.source.wait_for_path(
            source_path, settings.startup_timeout_seconds, publisher.process
        )
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
            lambda event: event.get("target_count") == "2",
        )
        assert _can_connect(port)

        for name, path in (
            ("first-a", "live/first"),
            ("first-b", "live/first"),
            ("second", "live/second"),
        ):
            probe = MediaProbe(
                e2e_config,
                "rtsp",
                f"rtsp://127.0.0.1:{port}/{path}",
                asset,
                3.0,
                artifact_directory,
                f"probe-{name}",
                stream_copy=True,
            )
            probes.append(probe)
            probe.start()
        for probe in probes:
            probe.wait(settings.startup_timeout_seconds + 5.0)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        for probe in probes:
            probe.stop()
        publisher.stop()
        runner.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    remux = final_performance(events, "remux")
    assert len(remux) == 2
    assert len({event["node_id"] for event in remux}) == 2
    assert all(int(event["output_count"]) > 0 for event in remux)
    assert all(event["failed_calls"] == "0" for event in remux)


@pytest.mark.sync
@pytest.mark.parametrize("scenario", ["rtsp_publish", "streaming"])
def test_rtsp_publish_pull_preserves_av_sync(
    scenario: str,
    sync_media_asset: MediaAsset,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    port = allocate_tcp_port()
    source_path = f"sync/rtsp-source-{artifact_directory.name[-8:]}"
    publish_path = f"sync/local-{artifact_directory.name[-8:]}"
    publisher = MediaPublisher(
        e2e_config,
        sync_media_asset,
        media_environment.source.publish_url("srt", source_path)
        + "&pkt_size=1316",
        artifact_directory / "source",
    )
    runner = Runner(
        e2e_config,
        runner_path,
        sync_media_asset,
        media_environment.source.read_url("srt", source_path)
        + "&pkt_size=1316",
        [],
        settings.startup_timeout_seconds + _SYNC_RECORD_SECONDS + 20.0,
        artifact_directory,
        scenario=scenario,
        cache_duration_ms=0,
        passthrough_video=scenario == "streaming",
        rtsp_publish_paths=[publish_path],
        rtsp_port=port,
    )
    recording = artifact_directory / "rtsp-pull-sync.mkv"
    recorder = MediaRecorder(
        e2e_config,
        "rtsp",
        f"rtsp://127.0.0.1:{port}/{publish_path}",
        sync_media_asset,
        _SYNC_RECORD_SECONDS,
        recording,
        artifact_directory / "recorder",
    )

    publisher.start()
    try:
        media_environment.source.wait_for_path(
            source_path, settings.startup_timeout_seconds, publisher.process
        )
        time.sleep(1.0)
        publisher.process.ensure_running()
        runner.start()
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
            lambda event: event.get("target_count") == "1",
        )
        recorder.start()
        recorder.wait(
            settings.startup_timeout_seconds + _SYNC_RECORD_SECONDS + 5.0
        )
        runner.process.ensure_running()
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        recorder.stop()
        runner.stop()
        publisher.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    remux = final_performance(events, "remux")
    assert len(remux) == 1
    assert int(remux[0]["output_count"]) > 0
    analysis = analyze_sync(
        e2e_config,
        recording,
        artifact_directory / "sync-analysis-rtsp-pull.json",
        ignore_before_seconds=_SYNC_IGNORE_BEFORE_SECONDS,
    )
    assert len(analysis.offsets) >= 10
