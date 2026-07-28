from __future__ import annotations

import pytest

from mw_e2e.events import assert_packet_flow, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, PublishedMedia
from mw_e2e.runner import Runner


@pytest.mark.fault
@pytest.mark.parametrize("output_protocol", ["rtsp", "rtmp", "srt"])
def test_output_reconnect(
    output_protocol: str,
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    sink = media_environment.sinks[output_protocol]
    output_path = f"{published_media.path}-reconnect-{output_protocol}"
    runner = Runner(
        e2e_config,
        runner_path,
        media_environment.source.read_url("srt", published_media.path),
        [sink.publish_url(output_protocol, output_path)],
        settings.startup_timeout_seconds * 2
        + settings.reconnect_timeout_seconds
        + settings.stability_seconds,
        artifact_directory,
    )
    runner.start()
    probe = None
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
        )
        sink.wait_for_path(output_path, settings.startup_timeout_seconds)
        connection = sink.wait_for_connection(
            output_protocol,
            output_path,
            "publish",
            settings.startup_timeout_seconds,
        )
        old_connection_id = str(connection["id"])
        sink.kick_connection(output_protocol, old_connection_id)
        sink.wait_for_connection(
            output_protocol,
            output_path,
            "publish",
            settings.reconnect_timeout_seconds,
            excluded_id=old_connection_id,
        )
        sink.wait_for_path(output_path, settings.startup_timeout_seconds)

        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            f"probe-{output_protocol}-after-reconnect",
        )
        probe.start()
        probe.wait(
            settings.startup_timeout_seconds + settings.stability_seconds
        )
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
