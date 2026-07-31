from __future__ import annotations

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe, MediaPublisher
from mw_e2e.mediamtx import allocate_tcp_port, allocate_udp_port
from mw_e2e.models import E2EConfig, MediaAsset
from mw_e2e.runner import Runner


def assert_clean_video_start(probe: MediaProbe) -> None:
    log = probe.process.log_path.read_text(encoding="utf-8", errors="replace")
    decoder_errors = (
        "PPS changed between slices",
        "Skipping invalid undecodable NALU",
        "Could not find ref with POC",
    )
    found = [error for error in decoder_errors if error in log]
    assert not found, f"输出视频不是从可解码边界开始: {found}"


@pytest.mark.cache
@pytest.mark.stability
def test_cache_duration(
    cache_duration_ms: int,
    media_asset: MediaAsset,
    e2e_config: E2EConfig,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    cache_duration_seconds = cache_duration_ms / 1000.0
    input_port = allocate_udp_port()
    input_url = f"srt://127.0.0.1:{input_port}?mode=caller"
    publish_url = (
        f"srt://127.0.0.1:{input_port}"
        "?mode=listener&pkt_size=1316"
    )
    output_url = f"rtsp://127.0.0.1:{allocate_tcp_port()}/cache"
    publisher = MediaPublisher(
        e2e_config,
        media_asset,
        publish_url,
        artifact_directory,
    )
    runner = Runner(
        e2e_config,
        runner_path,
        media_asset,
        input_url,
        [output_url],
        cache_duration_seconds
        + settings.startup_timeout_seconds * 2
        + settings.stability_seconds,
        artifact_directory,
        cache_duration_ms=cache_duration_ms,
    )
    probe = MediaProbe(
        e2e_config,
        "rtsp",
        output_url,
        media_asset,
        settings.stability_seconds,
        artifact_directory,
        "probe-cache",
        listen=True,
        listen_timeout_seconds=(
            cache_duration_seconds + settings.startup_timeout_seconds * 2
        ),
    )
    publisher.start()
    probe.start()
    runner.start()
    try:
        processor_started = wait_for_event(
            runner.process,
            runner.events_path,
            "processor_started",
            settings.startup_timeout_seconds,
        )
        running = wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            cache_duration_seconds + settings.reconnect_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        buffered_ms = int(running["ts_ms"]) - int(processor_started["ts_ms"])
        if cache_duration_ms == 0:
            assert buffered_ms <= 500, (
                f"0 缓存输出延迟过大: {buffered_ms}ms > 500ms"
            )
        else:
            assert buffered_ms >= cache_duration_ms - 250, (
                f"缓存过早开始输出: {buffered_ms}ms < "
                f"{cache_duration_ms - 250}ms"
            )

        probe.wait(
            cache_duration_seconds
            + settings.startup_timeout_seconds * 2
            + settings.stability_seconds
        )
        assert_clean_video_start(probe)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        probe.stop()
        runner.stop()
        publisher.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))
