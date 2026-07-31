from __future__ import annotations

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, PublishedMedia
from mw_e2e.runner import Runner


@pytest.mark.stability
def test_stable_multi_push(
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    output_paths = {
        protocol: f"{published_media.path}-stable-multi-{protocol}"
        for protocol in ("rtsp", "rtmp", "srt")
    }
    runner = Runner(
        e2e_config,
        runner_path,
        published_media.asset,
        media_environment.source.read_url("srt", published_media.path),
        [
            media_environment.sinks[protocol].publish_url(
                protocol, output_paths[protocol]
            )
            for protocol in ("rtsp", "rtmp", "srt")
        ],
        settings.startup_timeout_seconds * 2 + settings.stability_seconds,
        artifact_directory,
    )
    runner.start()
    probes: list[MediaProbe] = []
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
        )
        for protocol, sink in media_environment.sinks.items():
            sink.wait_for_path(
                output_paths[protocol], settings.startup_timeout_seconds
            )
            probe = MediaProbe(
                e2e_config,
                "rtsp",
                sink.read_url("rtsp", output_paths[protocol]),
                published_media.asset,
                settings.stability_seconds,
                artifact_directory,
                f"probe-{protocol}",
            )
            probes.append(probe)

        for probe in probes:
            probe.start()
        for probe in probes:
            probe.wait(
                settings.startup_timeout_seconds
                + settings.stability_seconds
            )
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        for probe in probes:
            probe.stop()
        runner.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))


@pytest.mark.fault
@pytest.mark.stability
def test_multi_push_isolates_and_recovers_one_failed_target(
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    output_paths = {
        protocol: f"{published_media.path}-multi-{protocol}"
        for protocol in ("rtsp", "rtmp", "srt")
    }
    output_urls = [
        media_environment.sinks[protocol].publish_url(
            protocol, output_paths[protocol]
        )
        for protocol in ("rtsp", "rtmp", "srt")
    ]
    runner = Runner(
        e2e_config,
        runner_path,
        published_media.asset,
        media_environment.source.read_url("srt", published_media.path),
        output_urls,
        settings.startup_timeout_seconds * 3
        + settings.reconnect_timeout_seconds
        + settings.stability_seconds,
        artifact_directory,
    )
    runner.start()
    probes: list[MediaProbe] = []
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
        )
        for protocol, sink in media_environment.sinks.items():
            sink.wait_for_path(
                output_paths[protocol], settings.startup_timeout_seconds
            )

        initial_rtmp_probe = MediaProbe(
            e2e_config,
            "rtsp",
            media_environment.sinks["rtmp"].read_url(
                "rtsp", output_paths["rtmp"]
            ),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            "probe-rtmp-before-fault",
        )
        probes.append(initial_rtmp_probe)
        initial_rtmp_probe.start()
        initial_rtmp_probe.wait(
            settings.startup_timeout_seconds + settings.stability_seconds
        )

        for protocol in ("rtsp", "srt"):
            probe = MediaProbe(
                e2e_config,
                "rtsp",
                media_environment.sinks[protocol].read_url(
                    "rtsp", output_paths[protocol]
                ),
                published_media.asset,
                settings.stability_seconds,
                artifact_directory,
                f"probe-{protocol}-during-fault",
            )
            probes.append(probe)
            probe.start()

        rtmp_sink = media_environment.sinks["rtmp"]
        connection = rtmp_sink.wait_for_connection(
            "rtmp",
            output_paths["rtmp"],
            "publish",
            settings.startup_timeout_seconds,
        )
        old_connection_id = str(connection["id"])
        rtmp_sink.kick_connection("rtmp", old_connection_id)
        rtmp_sink.wait_for_connection(
            "rtmp",
            output_paths["rtmp"],
            "publish",
            settings.reconnect_timeout_seconds,
            excluded_id=old_connection_id,
        )

        recovered_probe = MediaProbe(
            e2e_config,
            "rtsp",
            rtmp_sink.read_url("rtsp", output_paths["rtmp"]),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            "probe-rtmp-after-fault",
        )
        probes.append(recovered_probe)
        recovered_probe.start()
        recovered_probe.wait(
            settings.startup_timeout_seconds + settings.stability_seconds
        )

        for probe in probes:
            if probe not in {initial_rtmp_probe, recovered_probe}:
                probe.wait(
                    settings.startup_timeout_seconds
                    + settings.stability_seconds
                )
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        for probe in probes:
            probe.stop()
        runner.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))
