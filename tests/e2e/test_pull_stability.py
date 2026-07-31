from __future__ import annotations

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
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
    sink = media_environment.sinks["rtsp"]
    output_path = f"{published_media.path}-stable-pull-{input_protocol}"
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
        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            f"probe-stable-pull-{input_protocol}",
        )
        probe.start()
        probe.wait(settings.startup_timeout_seconds + settings.stability_seconds)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))
