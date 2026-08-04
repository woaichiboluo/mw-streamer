from __future__ import annotations

from pathlib import Path

import pytest

from mw_e2e.config import probe_media_file
from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, MediaAsset, PublishedMedia
from mw_e2e.runner import Runner
from mw_e2e.sync import find_recording


def _assert_source_spec(recording: MediaAsset, source: MediaAsset) -> None:
    assert recording.audio_codec == source.audio_codec
    assert recording.video_codec == source.video_codec
    assert recording.video_width == source.video_width
    assert recording.video_height == source.video_height


@pytest.mark.smoke
def test_streaming_pipeline_records_input_without_processed_output(
    e2e_config: E2EConfig,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    sample_path = Path(__file__).parents[1] / "data" / "h264_aac.mp4"
    asset = probe_media_file(e2e_config, sample_path)
    recording_target = artifact_directory / "streaming-input-only.mp4"
    runner = Runner(
        e2e_config,
        runner_path,
        asset,
        str(sample_path),
        [],
        e2e_config.tests.startup_timeout_seconds * 2,
        artifact_directory,
        input_output_urls=[str(recording_target)],
        software_video=True,
    )

    runner.start()
    try:
        runner.wait(e2e_config.tests.startup_timeout_seconds * 2)
    finally:
        runner.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    ready_events = [
        event for event in events if event.get("event") == "output_opened"
    ]
    assert ready_events[-1]["target_count"] == "0"
    assert ready_events[-1]["input_target_count"] == "1"
    summary = [event for event in events if event.get("event") == "summary"][-1]
    assert int(summary["video_encode_frames"]) > 0
    assert int(summary["audio_encode_samples"]) > 0

    recording = probe_media_file(e2e_config, find_recording(recording_target))
    _assert_source_spec(recording, asset)
    assert recording.duration_seconds >= 1.5


@pytest.mark.smoke
@pytest.mark.stability
def test_remux_pipeline_simultaneously_pushes_and_records_source(
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    sink = media_environment.sinks["rtsp"]
    output_path = f"{published_media.path}-remux"
    recording_target = artifact_directory / "remux-source.mp4"
    runner = Runner(
        e2e_config,
        runner_path,
        published_media.asset,
        media_environment.source.read_url("srt", published_media.path),
        [
            sink.publish_url("rtsp", output_path),
            str(recording_target),
        ],
        settings.startup_timeout_seconds * 3 + settings.stability_seconds,
        artifact_directory,
        pipeline="remux",
    )
    probe: MediaProbe | None = None
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
            lambda event: event.get("target_count") == "2",
        )
        sink.wait_for_path(
            output_path, settings.startup_timeout_seconds, runner.process
        )
        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            published_media.asset,
            settings.stability_seconds,
            artifact_directory,
            "probe-remux-output",
        )
        probe.start()
        probe.wait(settings.startup_timeout_seconds + settings.stability_seconds)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    assert all(event.get("event") != "processor_started" for event in events)
    recording = probe_media_file(e2e_config, find_recording(recording_target))
    _assert_source_spec(recording, published_media.asset)
    assert recording.duration_seconds >= settings.stability_seconds - 1.0


@pytest.mark.smoke
@pytest.mark.stability
def test_streaming_pipeline_input_outputs_push_and_record_with_processed_output(
    published_media: PublishedMedia,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    sink = media_environment.sinks["rtsp"]
    source_output_path = f"{published_media.path}-source-output"
    processed_output_path = f"{published_media.path}-processed-output"
    recording_target = artifact_directory / "streaming-source.mp4"
    runner = Runner(
        e2e_config,
        runner_path,
        published_media.asset,
        media_environment.source.read_url("srt", published_media.path),
        [sink.publish_url("rtsp", processed_output_path)],
        settings.startup_timeout_seconds * 3 + settings.stability_seconds,
        artifact_directory,
        input_output_urls=[
            sink.publish_url("rtsp", source_output_path),
            str(recording_target),
        ],
    )
    probes: list[MediaProbe] = []
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "output_opened",
            settings.startup_timeout_seconds,
            lambda event: event.get("target_count") == "1"
            and event.get("input_target_count") == "2",
        )
        wait_for_event(
            runner.process,
            runner.events_path,
            "processor_started",
            settings.startup_timeout_seconds,
        )
        sink.wait_for_path(
            source_output_path, settings.startup_timeout_seconds, runner.process
        )
        sink.wait_for_path(
            processed_output_path,
            settings.startup_timeout_seconds,
            runner.process,
        )
        for name, path in (
            ("source", source_output_path),
            ("processed", processed_output_path),
        ):
            probe = MediaProbe(
                e2e_config,
                "rtsp",
                sink.read_url("rtsp", path),
                published_media.asset,
                settings.stability_seconds,
                artifact_directory,
                f"probe-streaming-{name}",
            )
            probes.append(probe)
            probe.start()
        for probe in probes:
            probe.wait(
                settings.startup_timeout_seconds + settings.stability_seconds
            )
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        for probe in probes:
            probe.stop()
        runner.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))
    recording = probe_media_file(e2e_config, find_recording(recording_target))
    _assert_source_spec(recording, published_media.asset)
    assert recording.duration_seconds >= settings.stability_seconds - 1.0
