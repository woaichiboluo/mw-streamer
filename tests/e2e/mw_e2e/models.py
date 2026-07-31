from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ToolPaths:
    ffmpeg: Path
    ffprobe: Path
    mediamtx: Path


@dataclass(frozen=True)
class TestSettings:
    cache_duration_ms: int
    cache_durations_ms: tuple[int, ...]
    startup_timeout_seconds: float
    stability_seconds: float
    reconnect_timeout_seconds: float


@dataclass(frozen=True)
class E2EConfig:
    tools: ToolPaths
    media_directory: Path
    tests: TestSettings


@dataclass(frozen=True)
class MediaAsset:
    path: Path
    identifier: str
    duration_seconds: float
    audio_codec: str | None
    video_codec: str | None
    video_width: int | None
    video_height: int | None
    video_frame_rate_num: int | None
    video_frame_rate_den: int | None

    @property
    def has_audio(self) -> bool:
        return self.audio_codec is not None

    @property
    def has_video(self) -> bool:
        return self.video_codec is not None


@dataclass(frozen=True)
class PublishedMedia:
    asset: MediaAsset
    path: str
