from __future__ import annotations

import pytest

from mw_e2e.events import assert_packet_flow, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, PublishedMedia
from mw_e2e.runner import Runner


@pytest.mark.cache
@pytest.mark.stability
def test_cache_duration(
    cache_duration_ms: int,
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    cache_duration_seconds = cache_duration_ms / 1000.0
    sink = media_environment.sinks["rtsp"]
    output_path = f"{published_media.path}-cache-{cache_duration_ms}"
    runner = Runner(
        e2e_config,
        runner_path,
        media_environment.source.read_url("srt", published_media.path),
        [sink.publish_url("rtsp", output_path)],
        cache_duration_seconds
        + settings.startup_timeout_seconds * 2
        + settings.stability_seconds,
        artifact_directory,
        cache_duration_ms=cache_duration_ms,
    )
    runner.start()
    probe = None
    try:
        streams_ready = wait_for_event(
            runner.process,
            runner.events_path,
            "streams_ready",
            settings.startup_timeout_seconds,
        )
        playing = wait_for_event(
            runner.process,
            runner.events_path,
            "queue_state",
            cache_duration_seconds + settings.reconnect_timeout_seconds,
            lambda event: event.get("state") == "playing"
            and event.get("generation") == streams_ready.get("generation"),
        )
        buffered_ms = int(playing["ts_ms"]) - int(streams_ready["ts_ms"])
        assert buffered_ms >= cache_duration_ms - 250, (
            f"PacketQueue 过早开始输出: {buffered_ms}ms < "
            f"{cache_duration_ms - 250}ms"
        )

        sink.wait_for_path(output_path, settings.startup_timeout_seconds)
        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            "probe-cache",
        )
        probe.start()
        probe.wait(settings.startup_timeout_seconds + settings.stability_seconds)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()

    assert_packet_flow(
        read_events(runner.events_path),
        has_audio=published_media.asset.has_audio,
        has_video=published_media.asset.has_video,
        stall_timeout_seconds=settings.stall_timeout_seconds,
    )
