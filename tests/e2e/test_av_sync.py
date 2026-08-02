from __future__ import annotations

import hashlib
import time
from pathlib import Path

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe, MediaPublisher, MediaRecorder
from mw_e2e.mediamtx import (
    MediaEnvironment,
    MediaMtx,
    allocate_tcp_port,
    allocate_udp_port,
)
from mw_e2e.models import E2EConfig, MediaAsset
from mw_e2e.process import ProcessError
from mw_e2e.runner import Runner
from mw_e2e.sync import analyze_sync, concatenate_recordings, find_recording


_LONG_SYNC_DURATION_SECONDS = 300.0
_SYNC_IGNORE_BEFORE_SECONDS = 15.0
_MINIMUM_LONG_SYNC_MARKERS = 135
_JITTER_DURATION_SECONDS = 30.0
_STANDBY_RECOVERY_SECONDS = 20.0
_NETWORK_SOURCE_WARMUP_SECONDS = 1.0
_RTSP_SOURCE_WARMUP_SECONDS = 6.0
_INPUT_PROTOCOLS = ("file", "rtmp", "rtsp", "srt")
_OUTPUT_PROTOCOLS = ("file", "rtmp", "rtsp", "srt")


def _stream_url(protocol: str, url: str) -> str:
    if protocol == "srt":
        return url + "&pkt_size=1316"
    return url


def _progress(probe: MediaProbe) -> tuple[int, int]:
    values = probe.read_progress()
    media_time = int(
        values.get("out_time_us", values.get("out_time_ms", "0"))
    )
    return int(values.get("frame", "0")), media_time


def _wait_for_progress(
    probe: MediaProbe,
    minimum_frame: int,
    minimum_media_time_us: int,
    timeout_seconds: float,
) -> tuple[int, int]:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        probe.process.ensure_running()
        if probe.progress_path.exists():
            frame, media_time = _progress(probe)
            if (
                frame >= minimum_frame
                and media_time >= minimum_media_time_us
            ):
                return frame, media_time
        time.sleep(0.1)
    raise ProcessError(
        "等待音画同步探针进度超时: "
        f"frame>={minimum_frame}, media_time_us>={minimum_media_time_us}"
    )


def _verify_recording(
    config: E2EConfig,
    requested_recording: Path,
    artifact_directory: Path,
) -> None:
    recording = find_recording(requested_recording)
    analyze_sync(
        config,
        recording,
        artifact_directory / "sync-analysis.json",
        ignore_before_seconds=_SYNC_IGNORE_BEFORE_SECONDS,
    )


@pytest.mark.sync
@pytest.mark.stability
@pytest.mark.parametrize("input_protocol", _INPUT_PROTOCOLS)
def test_av_sync_protocol_matrix(
    input_protocol: str,
    sync_media_asset: MediaAsset,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    digest = hashlib.sha1(
        str(artifact_directory).encode(), usedforsecurity=False
    ).hexdigest()[:12]
    requested_recording = artifact_directory / "sync-output.mp4"
    input_url = str(sync_media_asset.path)
    publisher = None
    if input_protocol != "file":
        source_path = f"sync/source-{input_protocol}-{digest}"
        publish_url = _stream_url(
            input_protocol,
            media_environment.source.publish_url(input_protocol, source_path),
        )
        publisher = MediaPublisher(
            e2e_config,
            sync_media_asset,
            publish_url,
            artifact_directory / "source",
        )
        publisher.start()
        media_environment.source.wait_for_path(
            source_path,
            settings.startup_timeout_seconds,
            publisher.process,
        )
        source_warmup = (
            _RTSP_SOURCE_WARMUP_SECONDS
            if input_protocol == "rtsp"
            else _NETWORK_SOURCE_WARMUP_SECONDS
        )
        time.sleep(source_warmup)
        publisher.process.ensure_running()
        input_url = _stream_url(
            input_protocol,
            media_environment.source.read_url(input_protocol, source_path),
        )

    output_paths = {
        protocol: f"sync/{input_protocol}-to-{protocol}-{digest}"
        for protocol in _OUTPUT_PROTOCOLS
        if protocol != "file"
    }
    output_servers = {
        protocol: MediaMtx(
            e2e_config,
            f"{protocol}-sink",
            artifact_directory / "output",
            {protocol},
            record=True,
        )
        for protocol in ("rtmp", "srt")
    }
    started_servers: list[MediaMtx] = []
    try:
        for server in output_servers.values():
            server.start()
            started_servers.append(server)
    except Exception:
        for server in reversed(started_servers):
            server.stop()
        if publisher is not None:
            publisher.stop()
        raise
    rtsp_url = (
        f"rtsp://127.0.0.1:{allocate_tcp_port()}/"
        f"{output_paths['rtsp']}"
    )
    rtsp_recording = artifact_directory / "sync-playback-rtsp.mkv"
    rtsp_recorder = MediaRecorder(
        e2e_config,
        "rtsp",
        rtsp_url,
        sync_media_asset,
        _LONG_SYNC_DURATION_SECONDS,
        rtsp_recording,
        artifact_directory / "recorder-rtsp",
        listen=True,
        listen_timeout_seconds=settings.startup_timeout_seconds,
    )
    output_urls = [
        str(requested_recording),
        _stream_url(
            "rtmp",
            output_servers["rtmp"].publish_url(
                "rtmp", output_paths["rtmp"]
            ),
        ),
        rtsp_url,
        _stream_url(
            "srt",
            output_servers["srt"].publish_url("srt", output_paths["srt"]),
        ),
    ]
    runner = Runner(
        e2e_config,
        runner_path,
        sync_media_asset,
        input_url,
        output_urls,
        (
            settings.startup_timeout_seconds
            + _LONG_SYNC_DURATION_SECONDS
            + 30.0
        ),
        artifact_directory,
        cache_duration_ms=0,
        passthrough_video=True,
    )
    try:
        rtsp_recorder.start()
        runner.start()
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        for protocol, sink in output_servers.items():
            output_path = output_paths[protocol]
            sink.wait_for_path(
                output_path,
                settings.startup_timeout_seconds,
                runner.process,
            )
        rtsp_recorder.wait(
            settings.startup_timeout_seconds
            + _LONG_SYNC_DURATION_SECONDS
            + 5.0
        )
        runner.process.ensure_running()
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        runner.stop()
        rtsp_recorder.stop()
        for server in reversed(started_servers):
            server.stop()
        if publisher is not None:
            publisher.stop()

    events = read_events(runner.events_path)
    assert_pipeline_succeeded(events)
    recordings = {
        "file": find_recording(requested_recording),
        "rtsp": rtsp_recording,
        **{
            protocol: concatenate_recordings(
                e2e_config,
                output_servers[protocol].recordings(output_path),
                artifact_directory / f"sync-playback-{protocol}.mkv",
            )
            for protocol, output_path in output_paths.items()
            if protocol in output_servers
        },
    }
    for protocol, recording in recordings.items():
        analysis = analyze_sync(
            e2e_config,
            recording,
            artifact_directory / f"sync-analysis-{protocol}.json",
            ignore_before_seconds=_SYNC_IGNORE_BEFORE_SECONDS,
        )
        assert len(analysis.offsets) >= _MINIMUM_LONG_SYNC_MARKERS, (
            f"{protocol}稳定窗口同步标记不足: "
            f"{len(analysis.offsets)} < {_MINIMUM_LONG_SYNC_MARKERS}"
        )


@pytest.mark.sync
@pytest.mark.parametrize("input_protocol", _INPUT_PROTOCOLS)
def test_video_jitter_av_sync(
    input_protocol: str,
    sync_media_asset: MediaAsset,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    digest = hashlib.sha1(
        str(artifact_directory).encode(), usedforsecurity=False
    ).hexdigest()[:12]
    requested_recording = artifact_directory / "sync-output.mp4"
    input_url = str(sync_media_asset.path)
    publisher = None
    if input_protocol != "file":
        source_path = f"sync/jitter-{input_protocol}-{digest}"
        publisher = MediaPublisher(
            e2e_config,
            sync_media_asset,
            _stream_url(
                input_protocol,
                media_environment.source.publish_url(
                    input_protocol, source_path
                ),
            ),
            artifact_directory / "source",
        )
        publisher.start()
        media_environment.source.wait_for_path(
            source_path,
            settings.startup_timeout_seconds,
            publisher.process,
        )
        if input_protocol == "rtsp":
            time.sleep(_RTSP_SOURCE_WARMUP_SECONDS)
            publisher.process.ensure_running()
        input_url = _stream_url(
            input_protocol,
            media_environment.source.read_url(input_protocol, source_path),
        )

    runner = Runner(
        e2e_config,
        runner_path,
        sync_media_asset,
        input_url,
        [str(requested_recording)],
        settings.startup_timeout_seconds + _JITTER_DURATION_SECONDS + 5.0,
        artifact_directory,
        cache_duration_ms=0,
        passthrough_video=True,
        video_jitter_ms=(100, 200),
    )
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        deadline = time.monotonic() + _JITTER_DURATION_SECONDS
        while time.monotonic() < deadline:
            runner.process.ensure_running()
            time.sleep(0.1)
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        runner.stop()
        if publisher is not None:
            publisher.stop()

    events = read_events(runner.events_path)
    assert any(event.get("event") == "video_jitter" for event in events)
    assert_pipeline_succeeded(events)
    _verify_recording(e2e_config, requested_recording, artifact_directory)


@pytest.mark.sync
def test_standby_recovery_av_sync(
    sync_media_asset: MediaAsset,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path: Path,
    artifact_directory: Path,
) -> None:
    settings = e2e_config.tests
    digest = hashlib.sha1(
        str(artifact_directory).encode(), usedforsecurity=False
    ).hexdigest()[:12]
    source_path = f"sync/reconnect-{digest}"
    source_port = allocate_udp_port()
    source_url = (
        f"srt://127.0.0.1:{source_port}"
        "?mode=listener&transtype=live&pkt_size=1316"
    )
    input_url = (
        f"srt://127.0.0.1:{source_port}"
        "?mode=caller&transtype=live&pkt_size=1316"
    )
    publisher = MediaPublisher(
        e2e_config, sync_media_asset, source_url, artifact_directory
    )
    publisher.start()

    sink = media_environment.sinks["rtsp"]
    output_path = f"{source_path}-output"
    requested_recording = artifact_directory / "sync-output.mp4"
    runner = Runner(
        e2e_config,
        runner_path,
        sync_media_asset,
        input_url,
        [
            sink.publish_url("rtsp", output_path),
            str(requested_recording),
        ],
        (
            settings.startup_timeout_seconds
            + settings.reconnect_timeout_seconds
            + _STANDBY_RECOVERY_SECONDS
            + 6.0
        ),
        artifact_directory,
        passthrough_video=True,
        standby=True,
    )
    runner.start()
    probe = None
    replacement_publisher = None
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        sink.wait_for_path(
            output_path, settings.startup_timeout_seconds, runner.process
        )
        probe = MediaProbe(
            e2e_config,
            "rtsp",
            sink.read_url("rtsp", output_path),
            sync_media_asset,
            None,
            artifact_directory,
            "sync-standby-recovery",
        )
        probe.start()
        before_frame, before_time = _wait_for_progress(
            probe,
            25,
            1_000_000,
            settings.startup_timeout_seconds,
        )

        publisher.stop()
        _wait_for_progress(
            probe,
            before_frame + 50,
            before_time + 2_000_000,
            settings.reconnect_timeout_seconds,
        )

        replacement_publisher = MediaPublisher(
            e2e_config,
            sync_media_asset,
            source_url,
            artifact_directory / "replacement",
        )
        replacement_publisher.start()
        wait_for_event(
            runner.process,
            runner.events_path,
            "processor_boundary",
            settings.reconnect_timeout_seconds,
            lambda event: event.get("reason") == "timeline_reset",
        )
        current_frame, current_time = _progress(probe)
        _wait_for_progress(
            probe,
            current_frame + int(25 * _STANDBY_RECOVERY_SECONDS),
            current_time + int(_STANDBY_RECOVERY_SECONDS * 1_000_000),
            settings.startup_timeout_seconds + _STANDBY_RECOVERY_SECONDS,
        )
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        if probe is not None:
            probe.stop()
        runner.stop()
        publisher.stop()
        if replacement_publisher is not None:
            replacement_publisher.stop()

    events = read_events(runner.events_path)
    assert any(
        event.get("event") == "processor_boundary"
        and event.get("reason") == "timeline_reset"
        for event in events
    )
    assert_pipeline_succeeded(events)
    _verify_recording(e2e_config, requested_recording, artifact_directory)
