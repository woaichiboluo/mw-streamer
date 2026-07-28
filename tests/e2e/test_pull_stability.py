from __future__ import annotations

import pytest

from mw_e2e.events import assert_packet_flow, read_events, wait_for_event
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, PublishedMedia
from mw_e2e.runner import Runner


@pytest.mark.smoke
@pytest.mark.stability
@pytest.mark.parametrize("input_protocol", ["rtsp", "rtmp", "srt"])
def test_stable_pull(
    input_protocol: str,
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    duration = (
        settings.startup_timeout_seconds
        + settings.reconnect_timeout_seconds
        + settings.stability_seconds
    )
    runner = Runner(
        e2e_config,
        runner_path,
        media_environment.source.read_url(input_protocol, published_media.path),
        [],
        duration,
        artifact_directory,
    )
    runner.start()
    try:
        ready = wait_for_event(
            runner.process,
            runner.events_path,
            "player_state",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "ready",
        )
        generation = int(ready["generation"])
        flow_started = wait_for_event(
            runner.process,
            runner.events_path,
            "heartbeat",
            settings.reconnect_timeout_seconds,
            lambda event: int(event.get("generation", "0")) == generation
            and int(event.get("total_packets", "0")) > 0
            and (
                not published_media.asset.has_audio
                or int(event.get("audio_packets", "0")) > 0
            )
            and (
                not published_media.asset.has_video
                or int(event.get("video_packets", "0")) > 0
            ),
        )
        flow_started_ms = int(flow_started["ts_ms"])
        audio_packets = int(flow_started.get("audio_packets", "0"))
        video_packets = int(flow_started.get("video_packets", "0"))
        wait_for_event(
            runner.process,
            runner.events_path,
            "heartbeat",
            settings.stability_seconds + 2.0,
            lambda event: int(event.get("generation", "0")) == generation
            and int(event.get("ts_ms", "0"))
            >= flow_started_ms + settings.stability_seconds * 1000
            and (
                not published_media.asset.has_audio
                or int(event.get("audio_packets", "0")) > audio_packets
            )
            and (
                not published_media.asset.has_video
                or int(event.get("video_packets", "0")) > video_packets
            ),
        )
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        runner.stop()

    events = [
        event
        for event in read_events(runner.events_path)
        if event.get("event") == "summary"
        or int(event.get("ts_ms", "0")) >= flow_started_ms
    ]
    assert_packet_flow(
        events,
        has_audio=published_media.asset.has_audio,
        has_video=published_media.asset.has_video,
        stall_timeout_seconds=settings.stall_timeout_seconds,
    )
