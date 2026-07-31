from __future__ import annotations

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, PublishedMedia
from mw_e2e.runner import Runner


@pytest.mark.fault
@pytest.mark.parametrize("input_protocol", ["rtsp", "rtmp", "srt"])
def test_input_reconnect(
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
    sink = media_environment.sinks["rtsp"]
    output_path = f"{published_media.path}-input-reconnect-{input_protocol}"
    runner = Runner(
        e2e_config,
        runner_path,
        published_media.asset,
        media_environment.source.read_url(input_protocol, published_media.path),
        [sink.publish_url("rtsp", output_path)],
        duration,
        artifact_directory,
    )
    runner.start()
    probe = None
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        sink.wait_for_path(
            output_path,
            settings.startup_timeout_seconds,
            runner.process,
        )

        connection = media_environment.source.wait_for_connection(
            input_protocol,
            published_media.path,
            "read",
            settings.startup_timeout_seconds,
        )
        previous_timeline_reset_count = max(
            (
                int(event.get("timeline_reset_count", "0"))
                for event in read_events(runner.events_path)
                if event.get("event") == "processor_boundary"
            ),
            default=0,
        )
        media_environment.source.kick_connection(
            input_protocol, str(connection["id"])
        )
        media_environment.source.wait_for_connection(
            input_protocol,
            published_media.path,
            "read",
            settings.reconnect_timeout_seconds,
            excluded_id=str(connection["id"]),
        )
        wait_for_event(
            runner.process,
            runner.events_path,
            "processor_boundary",
            settings.reconnect_timeout_seconds,
            lambda event: event.get("reason") == "timeline_reset"
            and int(event.get("timeline_reset_count", "0"))
            > previous_timeline_reset_count,
        )
        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            f"probe-input-reconnect-{input_protocol}",
        )
        probe.start()
        probe.wait(settings.startup_timeout_seconds + settings.stability_seconds)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()

    events = read_events(runner.events_path)
    assert any(
        event.get("event") == "processor_boundary"
        and event.get("reason") == "timeline_reset"
        for event in events
    )
    assert_pipeline_succeeded(events)
