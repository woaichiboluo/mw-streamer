from __future__ import annotations

from pathlib import Path

import pytest

from mw_e2e.config import probe_media_file
from mw_e2e.events import assert_pipeline_succeeded, read_events
from mw_e2e.models import E2EConfig
from mw_e2e.runner import Runner


SAMPLES = (
    ("h264_aac.mp4", 20, 94, 96256),
    ("h265_aac.mp4", 20, 94, 96256),
    ("h264_video.mp4", 20, 0, 0),
)


def _event_index(events: list[dict[str, str]], name: str) -> int:
    return next(
        index for index, event in enumerate(events) if event.get("event") == name
    )


@pytest.mark.smoke
@pytest.mark.parametrize(
    (
        "sample_name",
        "expected_video_frames",
        "expected_audio_frames",
        "expected_audio_samples",
    ),
    SAMPLES,
    ids=[sample[0] for sample in SAMPLES],
)
def test_file_pipeline_processes_local_file_to_natural_eof(
    sample_name: str,
    expected_video_frames: int,
    expected_audio_frames: int,
    expected_audio_samples: int,
    e2e_config: E2EConfig,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    sample_path = Path(__file__).parents[1] / "data" / sample_name
    asset = probe_media_file(e2e_config, sample_path)
    runner = Runner(
        e2e_config,
        runner_path,
        asset,
        str(sample_path),
        [],
        e2e_config.tests.startup_timeout_seconds,
        artifact_directory,
        pipeline="file",
    )

    runner.start()
    try:
        runner.wait(e2e_config.tests.startup_timeout_seconds + 5.0)
    finally:
        runner.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    statuses = [
        event["state"]
        for event in events
        if event.get("event") == "pipeline_status"
    ]
    assert statuses == ["starting", "running", "stopped"]
    processor_started = [
        event for event in events if event.get("event") == "processor_started"
    ]
    assert len(processor_started) == 1
    assert processor_started[0]["has_audio"] == (
        "1" if asset.has_audio else "0"
    )
    assert processor_started[0]["has_video"] == (
        "1" if asset.has_video else "0"
    )
    assert processor_started[0]["execution"] == "cpu"
    boundaries = [
        event for event in events if event.get("event") == "processor_boundary"
    ]
    assert len(boundaries) == 1
    assert boundaries[0]["reason"] == "end_of_input"
    assert boundaries[0]["count"] == "1"
    assert len(
        [event for event in events if event.get("event") == "processor_stopped"]
    ) == 1
    summaries = [event for event in events if event.get("event") == "summary"]
    assert len(summaries) == 1
    summary = summaries[0]

    assert summary["timed_out"] == "0"
    assert int(summary["video_frames"]) == expected_video_frames
    assert int(summary["audio_frames"]) == expected_audio_frames
    assert int(summary["audio_samples"]) == expected_audio_samples
    assert int(summary["video_decode_frames"]) == expected_video_frames
    assert int(summary["video_process_frames"]) == expected_video_frames
    assert int(summary["audio_process_samples"]) == expected_audio_samples
    if asset.has_audio:
        assert int(summary["audio_decode_samples"]) > 0
    else:
        assert int(summary["audio_decode_samples"]) == 0
    assert summary["has_audio"] == ("1" if asset.has_audio else "0")
    assert summary["has_video"] == ("1" if asset.has_video else "0")
    assert summary["end_of_input_count"] == "1"
    assert summary["processor_stop_count"] == "1"
    assert summary["progress_available"] == "1"
    assert int(summary["duration_us"]) > 0
    assert summary["processed_position_us"] == summary["duration_us"]
    assert float(summary["progress"]) == pytest.approx(1.0)
    assert summary["processing_speed_available"] == "1"
    assert float(summary["processing_speed"]) > 0.0

    boundary_index = _event_index(events, "processor_boundary")
    processor_stopped_index = _event_index(events, "processor_stopped")
    stopped_status_index = next(
        index
        for index, event in enumerate(events)
        if event.get("event") == "pipeline_status"
        and event.get("state") == "stopped"
    )
    summary_index = _event_index(events, "summary")
    assert (
        boundary_index
        < processor_stopped_index
        < stopped_status_index
        < summary_index
    )
