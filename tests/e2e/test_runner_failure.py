from __future__ import annotations

from pathlib import Path

import pytest

from mw_e2e.events import final_performance, read_events
from mw_e2e.process import ManagedProcess


@pytest.mark.smoke
def test_processor_fatal_stops_pipeline_and_fails_runner(
    runner_path: Path, artifact_directory: Path
) -> None:
    sample = Path(__file__).parents[1] / "data" / "h264_aac.mp4"
    events_path = artifact_directory / "runner.events"
    # With no video callback, the 64x64 input cannot pass through a Processor
    # configured for 32x32. This must propagate fatal shutdown across the graph.
    process = ManagedProcess(
        [
            str(runner_path),
            "--scenario",
            "streaming",
            "--input",
            str(sample),
            "--events",
            str(events_path),
            "--duration-ms",
            "10000",
            "--cache-ms",
            "0",
            "--software-video",
            "--output-width",
            "32",
            "--output-height",
            "32",
            "--video-codec",
            "h264",
            "--output",
            str(artifact_directory / "fatal.mp4"),
        ],
        artifact_directory / "runner.log",
    )
    process.start()
    try:
        assert process.wait(15.0) == 2
    finally:
        process.stop()

    events = read_events(events_path)
    assert any(
        event.get("event") == "pipeline_error"
        and event.get("node_id") == "pipeline"
        for event in events
    )
    summary = [event for event in events if event.get("event") == "summary"]
    assert len(summary) == 1
    assert summary[0]["failed_seen"] == "1"
    assert summary[0]["final_status"] == "failed"
    assert len([e for e in events if e.get("event") == "processor_stopped"]) == 1
    decoders = final_performance(events, "video_decoder")
    assert len(decoders) == 1
    assert decoders[0]["in_flight"] == "0"
