from __future__ import annotations

import time
from pathlib import Path
from typing import Callable

from .process import ManagedProcess, ProcessError


Event = dict[str, str]


def read_events(path: Path) -> list[Event]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return []
    events: list[Event] = []
    for line in lines:
        event: Event = {}
        for field in line.split():
            key, separator, value = field.partition("=")
            if separator:
                event[key] = value
        if event:
            events.append(event)
    return events


def wait_for_event(
    process: ManagedProcess,
    path: Path,
    event_name: str,
    timeout: float,
    predicate: Callable[[Event], bool] | None = None,
) -> Event:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        process.ensure_running()
        for event in read_events(path):
            if event.get("event") == event_name and (
                predicate is None or predicate(event)
            ):
                return event
        time.sleep(0.1)
    raise ProcessError(f"等待 runner 事件超时: {event_name}: {path}")


def assert_packet_flow(
    events: list[Event],
    *,
    has_audio: bool,
    has_video: bool,
    stall_timeout_seconds: float,
) -> None:
    summaries = [event for event in events if event.get("event") == "summary"]
    if not summaries:
        raise AssertionError("runner 缺少 summary 事件")
    summary = summaries[-1]
    if int(summary.get("total_packets", "0")) <= 0:
        raise AssertionError("runner 没有输出任何数据包")
    if has_audio and int(summary.get("audio_packets", "0")) <= 0:
        raise AssertionError("runner 没有输出音频包")
    if has_video and int(summary.get("video_packets", "0")) <= 0:
        raise AssertionError("runner 没有输出视频包")

    heartbeats = [
        event for event in events if event.get("event") == "heartbeat"
    ]
    _assert_track_not_stalled(
        heartbeats, "audio_packets", has_audio, stall_timeout_seconds
    )
    _assert_track_not_stalled(
        heartbeats, "video_packets", has_video, stall_timeout_seconds
    )


def _assert_track_not_stalled(
    heartbeats: list[Event],
    field: str,
    required: bool,
    stall_timeout_seconds: float,
) -> None:
    if not required:
        return
    active = [event for event in heartbeats if int(event.get(field, "0")) > 0]
    if len(active) < 2:
        raise AssertionError(f"{field} 缺少足够的 heartbeat 样本")
    last_count = int(active[0][field])
    last_change_ms = int(active[0]["ts_ms"])
    for event in active[1:]:
        count = int(event[field])
        timestamp_ms = int(event["ts_ms"])
        if count > last_count:
            last_count = count
            last_change_ms = timestamp_ms
            continue
        if timestamp_ms - last_change_ms > stall_timeout_seconds * 1000:
            raise AssertionError(f"{field} 停顿超过 {stall_timeout_seconds} 秒")
