from __future__ import annotations

import hashlib
import re
from datetime import datetime, timezone
from pathlib import Path

import pytest

from mw_e2e.config import ConfigurationError, discover_media, load_config
from mw_e2e.ffmpeg import MediaPublisher
from mw_e2e.mediamtx import MediaEnvironment
from mw_e2e.models import E2EConfig, MediaAsset, PublishedMedia


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("mw-streamer-e2e")
    group.addoption(
        "--e2e-config",
        required=True,
        help="E2E TOML 配置文件的绝对或相对路径",
    )
    group.addoption(
        "--e2e-runner",
        required=True,
        help="mw_streamer_e2e_runner 的路径",
    )


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    needs_media = "media_asset" in metafunc.fixturenames
    needs_cache_duration = "cache_duration_ms" in metafunc.fixturenames
    if not needs_media and not needs_cache_duration:
        return
    config_path = metafunc.config.getoption("--e2e-config")
    try:
        config = load_config(config_path)
        assets = discover_media(config_path) if needs_media else ()
    except ConfigurationError as error:
        raise pytest.UsageError(str(error)) from error
    if needs_media:
        metafunc.parametrize(
            "media_asset",
            assets,
            ids=[asset.identifier for asset in assets],
        )
    if needs_cache_duration:
        durations = config.tests.cache_durations_ms
        metafunc.parametrize(
            "cache_duration_ms",
            durations,
            ids=[f"cache-{duration // 1000}s" for duration in durations],
        )


def pytest_report_header(config: pytest.Config) -> list[str]:
    config_path = config.getoption("--e2e-config")
    runner_path = config.getoption("--e2e-runner")
    return [f"E2E config: {config_path}", f"E2E runner: {runner_path}"]


@pytest.fixture(scope="session")
def e2e_config(pytestconfig: pytest.Config) -> E2EConfig:
    try:
        return load_config(pytestconfig.getoption("--e2e-config"))
    except ConfigurationError as error:
        raise pytest.UsageError(str(error)) from error


@pytest.fixture(scope="session")
def runner_path(pytestconfig: pytest.Config) -> Path:
    path = Path(pytestconfig.getoption("--e2e-runner")).expanduser().resolve()
    if not path.is_file():
        raise pytest.UsageError(f"E2E runner 不存在: {path}")
    if not path.stat().st_mode & 0o111:
        raise pytest.UsageError(f"E2E runner 不可执行: {path}")
    return path


@pytest.fixture(scope="session")
def artifact_root() -> Path:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = Path(__file__).parent / "artifacts" / timestamp
    root.mkdir(parents=True, exist_ok=False)
    return root


@pytest.fixture(scope="session")
def media_environment(
    e2e_config: E2EConfig, artifact_root: Path
) -> MediaEnvironment:
    environment = MediaEnvironment(e2e_config, artifact_root / "servers")
    environment.start()
    yield environment
    environment.stop()


@pytest.fixture
def artifact_directory(artifact_root: Path, request: pytest.FixtureRequest) -> Path:
    normalized = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.nodeid)
    digest = hashlib.sha1(request.node.nodeid.encode(), usedforsecurity=False).hexdigest()[
        :8
    ]
    path = artifact_root / f"{normalized[:120]}-{digest}"
    path.mkdir(parents=True, exist_ok=False)
    return path


@pytest.fixture
def published_media(
    e2e_config: E2EConfig,
    media_environment: MediaEnvironment,
    media_asset: MediaAsset,
    artifact_directory: Path,
    request: pytest.FixtureRequest,
) -> PublishedMedia:
    digest = hashlib.sha1(
        request.node.nodeid.encode(), usedforsecurity=False
    ).hexdigest()[:12]
    media_path = f"live/source-{digest}"
    publisher = MediaPublisher(
        e2e_config,
        media_asset,
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
        yield PublishedMedia(media_asset, media_path)
    finally:
        publisher.stop()
