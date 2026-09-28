from __future__ import annotations

import json
from pathlib import Path

import pytest

from mw_e2e import sync
from mw_e2e.models import E2EConfig
from mw_e2e.process import ProcessError


@pytest.mark.parametrize("offset", [-0.3, -0.041, 0.041, 0.3])
def test_sync_analysis_rejects_out_of_limit_marker(
    offset: float,
    e2e_config: E2EConfig,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video_markers = [1.0, 3.0, 5.0, 7.0]
    audio_markers = [1.0, 3.0 + offset, 5.0, 7.0]

    def detect(config: E2EConfig, path: Path, arguments: list[str]) -> str:
        video = arguments[0] == "-vf"
        markers = video_markers if video else audio_markers
        begin, end = (
            ("black_end", "black_start")
            if video
            else ("silence_end", "silence_start")
        )
        return "\n".join(
            f"{begin}:{marker} {end}:{marker + 0.12}"
            for marker in markers
        )

    monkeypatch.setattr(sync, "_run_detection", detect)
    analysis_path = tmp_path / "analysis.json"
    with pytest.raises(ProcessError, match="超过40ms"):
        sync.analyze_sync(e2e_config, tmp_path / "source.mp4", analysis_path)

    # The rejected marker remains in the diagnostic report instead of being
    # discarded while the other three aligned markers make the test pass.
    report = json.loads(analysis_path.read_text(encoding="utf-8"))
    assert len(report["offsets_ms"]) == 4
    assert report["offsets_ms"][1] == pytest.approx(offset * 1000)


def test_sync_analysis_matches_markers_before_ignoring_start(
    e2e_config: E2EConfig,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video_markers = [13.0, 15.0, 17.0, 19.0, 21.0]
    audio_markers = [marker - 0.005 for marker in video_markers]

    def detect(config: E2EConfig, path: Path, arguments: list[str]) -> str:
        video = arguments[0] == "-vf"
        markers = video_markers if video else audio_markers
        begin, end = (
            ("black_end", "black_start")
            if video
            else ("silence_end", "silence_start")
        )
        return "\n".join(
            f"{begin}:{marker} {end}:{marker + 0.12}"
            for marker in markers
        )

    monkeypatch.setattr(sync, "_run_detection", detect)
    analysis = sync.analyze_sync(
        e2e_config,
        tmp_path / "source.mp4",
        tmp_path / "analysis.json",
        ignore_before_seconds=15.0,
    )

    assert len(analysis.offsets) == 4
    assert analysis.offsets == pytest.approx((-0.005,) * 4)


def test_sync_analysis_skips_missing_first_audio_marker(
    e2e_config: E2EConfig,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video_markers = [1.0, 3.0, 5.0, 7.0, 9.0]
    audio_markers = [marker + 0.008 for marker in video_markers[1:]]

    def detect(config: E2EConfig, path: Path, arguments: list[str]) -> str:
        video = arguments[0] == "-vf"
        markers = video_markers if video else audio_markers
        begin, end = (
            ("black_end", "black_start")
            if video
            else ("silence_end", "silence_start")
        )
        return "\n".join(
            f"{begin}:{marker} {end}:{marker + 0.12}"
            for marker in markers
        )

    monkeypatch.setattr(sync, "_run_detection", detect)
    analysis = sync.analyze_sync(
        e2e_config,
        tmp_path / "source.mp4",
        tmp_path / "analysis.json",
    )

    assert len(analysis.offsets) == 4
    assert analysis.offsets == pytest.approx((0.008,) * 4)


def test_sync_analysis_rejects_whole_marker_period_shift(
    e2e_config: E2EConfig,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    video_markers = [
        2 * index + 0.759 + 0.04 * ((7 * index + 5 * (index // 13)) % 13)
        for index in range(10)
    ]
    audio_markers = [marker + 2.0 for marker in video_markers]

    def detect(config: E2EConfig, path: Path, arguments: list[str]) -> str:
        video = arguments[0] == "-vf"
        markers = video_markers if video else audio_markers
        begin, end = (
            ("black_end", "black_start")
            if video
            else ("silence_end", "silence_start")
        )
        return "\n".join(
            f"{begin}:{marker} {end}:{marker + 0.12}"
            for marker in markers
        )

    monkeypatch.setattr(sync, "_run_detection", detect)
    with pytest.raises(ProcessError, match="超过40ms"):
        sync.analyze_sync(
            e2e_config,
            tmp_path / "source.mp4",
            tmp_path / "analysis.json",
        )
