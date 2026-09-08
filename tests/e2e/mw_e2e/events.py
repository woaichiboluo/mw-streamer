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


def assert_pipeline_succeeded(events: list[Event]) -> None:
    if not any(
        event.get("event") == "runner_started"
        and event.get("pipeline_api") == "unified"
        for event in events
    ):
        raise AssertionError("runner 未使用新的统一 Pipeline，请重新构建运行器")
    summaries = [event for event in events if event.get("event") == "summary"]
    if not summaries:
        raise AssertionError("runner 缺少 summary 事件")
    summary = summaries[-1]
    if summary.get("running_seen") != "1":
        raise AssertionError("Pipeline 未进入 running 状态")
    if summary.get("failed_seen") != "0":
        raise AssertionError("Pipeline 运行期间进入 failed 状态")
    if summary.get("final_status") != "stopped":
        raise AssertionError(
            f"Pipeline 最终状态不是 stopped: {summary.get('final_status')}"
        )


def final_performance(
    events: list[Event], performance_type: str, *, node_id: str | None = None
) -> list[Event]:
    """Select owned node statistics emitted after Pipeline.Stop()."""
    return [
        event
        for event in events
        if event.get("event") == "performance"
        and event.get("phase") == "final"
        and event.get("type") == performance_type
        and (node_id is None or event.get("node_id") == node_id)
    ]
