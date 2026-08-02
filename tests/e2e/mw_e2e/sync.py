from __future__ import annotations

import bisect
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .models import E2EConfig, MediaAsset
from .process import ProcessError


_MARKER_PERIOD_SECONDS = 2
_MARKER_START_SECONDS = 1
_MARKER_DURATION_SECONDS = 0.12
_SYNC_MEDIA_DURATION_SECONDS = 330
_MINIMUM_MARKER_DURATION_SECONDS = 0.06
_MAXIMUM_MARKER_DURATION_SECONDS = 0.25
_AUDIO_LEAD_LIMIT_SECONDS = 0.040
_AUDIO_LAG_LIMIT_SECONDS = 0.040


@dataclass(frozen=True)
class SyncAnalysis:
    video_markers: tuple[float, ...]
    audio_markers: tuple[float, ...]
    offsets: tuple[float, ...]

    @property
    def maximum_audio_lead(self) -> float:
        return max((max(-offset, 0.0) for offset in self.offsets), default=0.0)

    @property
    def maximum_audio_lag(self) -> float:
        return max((max(offset, 0.0) for offset in self.offsets), default=0.0)


def generate_sync_media(
    config: E2EConfig, codec: str, output_directory: Path
) -> MediaAsset:
    if codec not in {"h264", "hevc"}:
        raise ValueError(f"未知同步媒体编码格式: {codec}")
    output_directory.mkdir(parents=True, exist_ok=True)
    path = output_directory / f"sync-{codec}.mp4"
    video_encoder = "libx264" if codec == "h264" else "libx265"
    marker_expression = (
        f"between(mod(t\\,{_MARKER_PERIOD_SECONDS})"
        f"\\,{_MARKER_START_SECONDS}"
        f"\\,{_MARKER_START_SECONDS + _MARKER_DURATION_SECONDS})"
    )
    # All-intra video prevents an arbitrary live join point from discarding
    # video until the next GOP while audio has already started.
    command = [
        str(config.tools.ffmpeg),
        "-hide_banner",
        "-nostdin",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"color=c=black:s=640x360:r=25:d={_SYNC_MEDIA_DURATION_SECONDS}",
        "-f",
        "lavfi",
        "-i",
        (
            "aevalsrc="
            f"0.8*sin(2*PI*1000*t)*{marker_expression}"
            f":s=48000:d={_SYNC_MEDIA_DURATION_SECONDS}"
        ),
        "-filter_complex",
        (
            "[0:v]drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:"
            f"enable={marker_expression},format=yuv420p[v]"
        ),
        "-map",
        "[v]",
        "-map",
        "1:a",
        "-c:v",
        video_encoder,
        "-preset",
        "veryfast",
        "-g",
        "1",
        "-bf",
        "0",
        "-c:a",
        "aac",
        "-b:a",
        "128k",
        "-movflags",
        "+faststart",
        "-shortest",
        "-y",
        str(path),
    ]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=300, check=False
    )
    if completed.returncode != 0:
        raise ProcessError(
            f"生成{codec}同步媒体失败: {completed.stderr.strip()}"
        )
    return MediaAsset(
        path=path,
        identifier=f"sync/{path.name}",
        duration_seconds=float(_SYNC_MEDIA_DURATION_SECONDS),
        audio_codec="aac",
        video_codec=codec,
        video_width=640,
        video_height=360,
        video_frame_rate_num=25,
        video_frame_rate_den=1,
    )


def find_recording(requested_path: Path) -> Path:
    candidates = sorted(
        requested_path.parent.glob(
            f"{requested_path.stem}_*{requested_path.suffix}"
        )
    )
    if len(candidates) != 1:
        raise ProcessError(
            f"期望得到一个同步录像，实际为{len(candidates)}个: "
            f"{requested_path.parent}"
        )
    return candidates[0]


def concatenate_recordings(
    config: E2EConfig, recordings: list[Path], output_path: Path
) -> Path:
    if not recordings:
        raise ValueError("至少需要一段录像")
    concat_path = output_path.with_suffix(".ffconcat")
    concat_lines = ["ffconcat version 1.0"]
    for recording in recordings:
        escaped_path = str(recording.resolve()).replace("'", "'\\''")
        concat_lines.append(f"file '{escaped_path}'")
    concat_path.write_text(
        "\n".join(concat_lines) + "\n",
        encoding="utf-8",
    )
    command = [
        str(config.tools.ffmpeg),
        "-hide_banner",
        "-nostdin",
        "-loglevel",
        "error",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        str(concat_path),
        "-c",
        "copy",
        "-y",
        str(output_path),
    ]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=180, check=False
    )
    if completed.returncode != 0:
        raise ProcessError(
            f"拼接同步录像失败: {completed.stderr.strip()}"
        )
    return output_path


def analyze_sync(
    config: E2EConfig,
    path: Path,
    analysis_path: Path,
    *,
    ignore_before_seconds: float = 0.0,
) -> SyncAnalysis:
    if ignore_before_seconds < 0:
        raise ValueError("同步分析忽略时长不能为负数")
    video_output = _run_detection(
        config,
        path,
        ["-vf", "blackdetect=d=0.04:pix_th=0.10", "-an"],
    )
    audio_output = _run_detection(
        config,
        path,
        ["-af", "silencedetect=n=-40dB:d=0.04", "-vn"],
    )
    video_markers = _bounded_intervals(
        video_output, "black_end", "black_start"
    )
    audio_markers = _bounded_intervals(
        audio_output, "silence_end", "silence_start"
    )
    video_markers = [
        marker for marker in video_markers if marker >= ignore_before_seconds
    ]
    audio_markers = [
        marker for marker in audio_markers if marker >= ignore_before_seconds
    ]
    offsets = _match_markers(video_markers, audio_markers)
    analysis = SyncAnalysis(
        tuple(video_markers), tuple(audio_markers), tuple(offsets)
    )
    analysis_path.write_text(
        json.dumps(
            {
                "ignore_before_seconds": ignore_before_seconds,
                "video_markers": video_markers,
                "audio_markers": audio_markers,
                "offsets_ms": [offset * 1000 for offset in offsets],
                "maximum_audio_lead_ms": analysis.maximum_audio_lead * 1000,
                "maximum_audio_lag_ms": analysis.maximum_audio_lag * 1000,
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )
    if len(offsets) < 3:
        raise ProcessError(
            f"同步标记数量不足: video={len(video_markers)}, "
            f"audio={len(audio_markers)}, matched={len(offsets)}"
        )
    if analysis.maximum_audio_lead > _AUDIO_LEAD_LIMIT_SECONDS:
        raise ProcessError(
            "稳定播放声音提前超过40ms: "
            f"{analysis.maximum_audio_lead * 1000:.3f}ms"
        )
    if analysis.maximum_audio_lag > _AUDIO_LAG_LIMIT_SECONDS:
        raise ProcessError(
            "稳定播放声音滞后超过40ms: "
            f"{analysis.maximum_audio_lag * 1000:.3f}ms"
        )
    return analysis


def _run_detection(
    config: E2EConfig, path: Path, filter_arguments: list[str]
) -> str:
    command = [
        str(config.tools.ffmpeg),
        "-hide_banner",
        "-nostdin",
        "-i",
        str(path),
        *filter_arguments,
        "-f",
        "null",
        "-",
    ]
    completed = subprocess.run(
        command, capture_output=True, text=True, timeout=180, check=False
    )
    if completed.returncode != 0:
        raise ProcessError(
            f"检测同步媒体失败: {path}: {completed.stderr.strip()}"
        )
    return completed.stderr


def _bounded_intervals(
    output: str, begin_name: str, end_name: str
) -> list[float]:
    events = sorted(
        (
            (float(match.group(2)), match.group(1))
            for match in re.finditer(
                rf"({begin_name}|{end_name}):\s*(-?\d+(?:\.\d+)?)",
                output,
            )
        ),
        key=lambda event: event[0],
    )
    starts: list[float] = []
    for index, (start, name) in enumerate(events):
        if name != begin_name:
            continue
        end = next(
            (
                timestamp
                for timestamp, following_name in events[index + 1 :]
                if following_name == end_name and timestamp > start
            ),
            None,
        )
        if end is None:
            continue
        duration = end - start
        if (
            _MINIMUM_MARKER_DURATION_SECONDS
            <= duration
            <= _MAXIMUM_MARKER_DURATION_SECONDS
        ):
            starts.append(start)
    return starts


def _match_markers(
    video_markers: list[float], audio_markers: list[float]
) -> list[float]:
    available = list(audio_markers)
    offsets: list[float] = []
    for video in video_markers:
        insertion = bisect.bisect_left(available, video)
        candidate_indices = [
            index
            for index in (insertion - 1, insertion)
            if 0 <= index < len(available)
        ]
        if not candidate_indices:
            continue
        index = min(
            candidate_indices, key=lambda value: abs(available[value] - video)
        )
        offset = available[index] - video
        if abs(offset) > _MAXIMUM_MARKER_DURATION_SECONDS:
            continue
        offsets.append(offset)
        available.pop(index)
    return offsets
