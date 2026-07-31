from __future__ import annotations

import hashlib
from collections.abc import Iterator
from pathlib import Path

import pytest

from mw_e2e.events import assert_pipeline_succeeded, read_events, wait_for_event
from mw_e2e.ffmpeg import MediaProbe, MediaPublisher, monitor_stable_probes
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, MediaAsset
from mw_e2e.process import ManagedProcess, ProcessError
from mw_e2e.runner import Runner


BENCH_DURATION_SECONDS = 10 * 60
INPUT_PROTOCOLS = ("file", "rtsp", "rtmp", "srt")
OUTPUT_PROTOCOLS = ("rtsp", "rtmp", "srt")


def prepare_file_input(
    e2e_config: E2EConfig,
    media_asset: MediaAsset,
    duration_seconds: float,
    artifact_directory: Path,
) -> Path:
    output_path = artifact_directory / "bench-input.mp4"
    command = [
        str(e2e_config.tools.ffmpeg),
        "-hide_banner",
        "-nostdin",
        "-loglevel",
        "info",
        "-stream_loop",
        "-1",
        "-i",
        str(media_asset.path),
        "-t",
        str(duration_seconds),
    ]
    if media_asset.has_video:
        command.extend(["-map", "0:v:0"])
    if media_asset.has_audio:
        command.extend(["-map", "0:a:0"])
    command.extend(["-c", "copy", str(output_path)])

    process = ManagedProcess(command, artifact_directory / "file-prepare.log")
    process.start()
    returncode = process.wait(e2e_config.tests.startup_timeout_seconds * 4)
    if returncode != 0:
        raise ProcessError(
            f"准备FILE Bench输入失败，返回码{returncode}: "
            f"{process.log_path}"
        )
    return output_path


@pytest.fixture
def bench_input_url(
    input_protocol: str,
    bench_media_asset: MediaAsset,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    artifact_directory: Path,
    request: pytest.FixtureRequest,
) -> Iterator[str]:
    if input_protocol == "file":
        file_duration = (
            BENCH_DURATION_SECONDS
            + e2e_config.tests.startup_timeout_seconds * 5
        )
        yield str(
            prepare_file_input(
                e2e_config,
                bench_media_asset,
                file_duration,
                artifact_directory,
            )
        )
        return

    digest = hashlib.sha1(
        request.node.nodeid.encode(), usedforsecurity=False
    ).hexdigest()[:12]
    media_path = f"bench/source-{digest}"
    publisher = MediaPublisher(
        e2e_config,
        bench_media_asset,
        media_environment.source.publish_url("srt", media_path)
        + "&pkt_size=1316",
        artifact_directory,
    )
    publisher.start()
    try:
        media_environment.source.wait_for_path(
            media_path,
            e2e_config.tests.startup_timeout_seconds,
            publisher.process,
        )
        yield media_environment.source.read_url(
            input_protocol, media_path
        )
    finally:
        publisher.stop()


@pytest.mark.bench
@pytest.mark.parametrize("input_protocol", INPUT_PROTOCOLS)
@pytest.mark.parametrize(
    "bench_media_asset",
    ("h264", "hevc"),
    indirect=True,
    ids=("h264", "h265"),
)
def test_pipeline_protocol_codec_bench(
    input_protocol: str,
    bench_media_asset: MediaAsset,
    bench_input_url: str,
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    runner_path,
    artifact_directory,
) -> None:
    settings = e2e_config.tests
    case_id = artifact_directory.name.rsplit("-", 1)[-1]
    output_paths = {
        protocol: f"bench/output-{case_id}-{protocol}"
        for protocol in OUTPUT_PROTOCOLS
    }
    runner = Runner(
        e2e_config,
        runner_path,
        bench_media_asset,
        bench_input_url,
        [
            media_environment.sinks[protocol].publish_url(
                protocol, output_paths[protocol]
            )
            for protocol in OUTPUT_PROTOCOLS
        ],
        BENCH_DURATION_SECONDS + settings.startup_timeout_seconds * 5,
        artifact_directory,
    )

    probes: list[MediaProbe] = []
    runner.start()
    try:
        wait_for_event(
            runner.process,
            runner.events_path,
            "pipeline_status",
            settings.startup_timeout_seconds,
            lambda event: event.get("state") == "running",
        )
        for protocol in OUTPUT_PROTOCOLS:
            sink = media_environment.sinks[protocol]
            sink.wait_for_path(
                output_paths[protocol],
                settings.startup_timeout_seconds,
                runner.process,
            )
            probes.append(
                MediaProbe(
                    e2e_config,
                    "rtsp",
                    sink.read_url("rtsp", output_paths[protocol]),
                    bench_media_asset,
                    None,
                    artifact_directory,
                    f"probe-{protocol}",
                    stream_copy=True,
                )
            )

        for probe in probes:
            probe.start()
        monitor_stable_probes(
            probes,
            BENCH_DURATION_SECONDS,
            settings.startup_timeout_seconds,
        )
        for probe in probes:
            probe.stop()
        runner.interrupt_and_wait(settings.startup_timeout_seconds)
    finally:
        for probe in probes:
            probe.stop()
        runner.stop()

    assert_pipeline_succeeded(read_events(runner.events_path))
