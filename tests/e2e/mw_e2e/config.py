from __future__ import annotations

import json
import os
import subprocess
import tomllib
from functools import lru_cache
from pathlib import Path
from typing import Any

from .models import E2EConfig, MediaAsset, TestSettings, ToolPaths


class ConfigurationError(RuntimeError):
    pass


def _require_table(document: dict[str, Any], name: str) -> dict[str, Any]:
    value = document.get(name)
    if not isinstance(value, dict):
        raise ConfigurationError(f"配置缺少 [{name}]")
    return value


def _require_path(table: dict[str, Any], key: str) -> Path:
    value = table.get(key)
    if not isinstance(value, str) or not value:
        raise ConfigurationError(f"配置项 {key} 必须是非空路径")
    return Path(value).expanduser().resolve()


def _require_positive_number(table: dict[str, Any], key: str) -> float:
    value = table.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        raise ConfigurationError(f"配置项 {key} 必须是正数")
    return float(value)


def _is_valid_cache_duration(value: object) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and (value == 0 or 1000 <= value <= 30000)
    )


def _require_cache_durations(table: dict[str, Any]) -> tuple[int, ...]:
    values = table.get("cache_durations_ms")
    if not isinstance(values, list) or not values:
        raise ConfigurationError("cache_durations_ms 必须是非空整数数组")
    if any(not _is_valid_cache_duration(value) for value in values):
        raise ConfigurationError(
            "cache_durations_ms 的每个值必须为 0，或 1000 到 30000 的整数"
        )
    if len(set(values)) != len(values):
        raise ConfigurationError("cache_durations_ms 不能包含重复值")
    return tuple(values)


def _validate_executable(path: Path, name: str) -> None:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ConfigurationError(f"{name} 不存在或不可执行: {path}")


@lru_cache(maxsize=None)
def load_config(path: str) -> E2EConfig:
    config_path = Path(path).expanduser().resolve()
    try:
        with config_path.open("rb") as config_file:
            document = tomllib.load(config_file)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ConfigurationError(f"无法读取 E2E 配置 {config_path}: {error}") from error

    tools_table = _require_table(document, "tools")
    media_table = _require_table(document, "media")
    tests_table = _require_table(document, "tests")

    tools = ToolPaths(
        ffmpeg=_require_path(tools_table, "ffmpeg"),
        ffprobe=_require_path(tools_table, "ffprobe"),
        mediamtx=_require_path(tools_table, "mediamtx"),
    )
    _validate_executable(tools.ffmpeg, "ffmpeg")
    _validate_executable(tools.ffprobe, "ffprobe")
    _validate_executable(tools.mediamtx, "mediamtx")

    media_directory = _require_path(media_table, "directory")
    if not media_directory.is_dir():
        raise ConfigurationError(f"媒体目录不存在: {media_directory}")

    cache_duration_ms = tests_table.get("cache_duration_ms")
    if not _is_valid_cache_duration(cache_duration_ms):
        raise ConfigurationError(
            "cache_duration_ms 必须为 0，或 1000 到 30000 的整数"
        )

    return E2EConfig(
        tools=tools,
        media_directory=media_directory,
        tests=TestSettings(
            cache_duration_ms=cache_duration_ms,
            cache_durations_ms=_require_cache_durations(tests_table),
            startup_timeout_seconds=_require_positive_number(
                tests_table, "startup_timeout_seconds"
            ),
            stability_seconds=_require_positive_number(
                tests_table, "stability_seconds"
            ),
            stall_timeout_seconds=_require_positive_number(
                tests_table, "stall_timeout_seconds"
            ),
            reconnect_timeout_seconds=_require_positive_number(
                tests_table, "reconnect_timeout_seconds"
            ),
        ),
    )


def _probe_media(config: E2EConfig, path: Path) -> MediaAsset:
    command = [
        str(config.tools.ffprobe),
        "-v",
        "error",
        "-show_entries",
        "stream=codec_type,codec_name:format=duration",
        "-of",
        "json",
        str(path),
    ]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=30, check=False
    )
    if completed.returncode != 0:
        raise ConfigurationError(
            f"ffprobe 无法读取媒体文件 {path}: {completed.stderr.strip()}"
        )

    try:
        metadata = json.loads(completed.stdout)
        streams = metadata["streams"]
        duration_seconds = float(metadata["format"]["duration"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"媒体文件元数据不完整: {path}") from error

    audio_codecs = [
        stream["codec_name"]
        for stream in streams
        if stream.get("codec_type") == "audio"
    ]
    video_codecs = [
        stream["codec_name"]
        for stream in streams
        if stream.get("codec_type") == "video"
    ]
    if not audio_codecs and not video_codecs:
        raise ConfigurationError(f"媒体文件没有音频或视频轨道: {path}")
    if len(audio_codecs) > 1 or len(video_codecs) > 1:
        raise ConfigurationError(f"首版每种媒体类型最多支持一条轨道: {path}")
    if audio_codecs and audio_codecs[0] != "aac":
        raise ConfigurationError(f"首版网络矩阵只支持 AAC 音频: {path}")
    if video_codecs and video_codecs[0] not in {"h264", "hevc"}:
        raise ConfigurationError(f"首版网络矩阵只支持 H.264/H.265 视频: {path}")
    if duration_seconds <= 0:
        raise ConfigurationError(f"媒体文件时长无效: {path}")

    relative_path = path.relative_to(config.media_directory)
    return MediaAsset(
        path=path,
        identifier=relative_path.as_posix(),
        duration_seconds=duration_seconds,
        audio_codec=audio_codecs[0] if audio_codecs else None,
        video_codec=video_codecs[0] if video_codecs else None,
    )


@lru_cache(maxsize=None)
def discover_media(config_path: str) -> tuple[MediaAsset, ...]:
    config = load_config(config_path)
    files = sorted(path for path in config.media_directory.rglob("*") if path.is_file())
    if not files:
        raise ConfigurationError(f"媒体目录中没有文件: {config.media_directory}")
    return tuple(_probe_media(config, path) for path in files)
