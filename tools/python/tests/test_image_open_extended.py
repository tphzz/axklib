from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path
from typing import Any

import pytest

from image_open_extended import bounded_command, run_bounded, summarize, worker_case


def test_worker_has_hard_address_space_core_and_wall_limits(tmp_path: Path) -> None:
    command = bounded_command(["python", "worker.py"])
    assert command[0] == "prlimit"
    assert "--as=4294967296" in command
    assert "--core=0" in command
    assert "--kill-after=3s" in command
    assert "120s" in command
    assert command[-2:] == ["python", "worker.py"]


@pytest.mark.skipif(
    sys.platform != "linux" or not shutil.which("prlimit"), reason="Linux worker limits"
)
def test_records_each_worker_exit_and_peak_rss(tmp_path: Path) -> None:
    log = tmp_path / "worker.log"
    result = run_bounded([sys.executable, "-c", "print('measured');raise SystemExit(7)"], log)
    assert result["exitStatus"] == 7
    assert result["peakRssKiB"] > 0
    assert result["elapsedSeconds"] > 0
    assert log.read_text().strip() == "measured"


@pytest.mark.skipif(
    sys.platform != "linux" or not shutil.which("prlimit"), reason="Linux worker limits"
)
def test_wall_timeout_is_a_measured_failure(tmp_path: Path) -> None:
    result = run_bounded(
        [sys.executable, "-c", "import time; time.sleep(20)"],
        tmp_path / "timeout.log",
        wall_seconds=1,
    )
    assert result["exitStatus"] == 124
    assert 1 <= result["elapsedSeconds"] < 5


def test_invalid_but_browsable_image_remains_a_failed_case(
    tmp_path: Path, monkeypatch: Any
) -> None:
    source = tmp_path / "disk.img"
    source.write_bytes(b"fixture")

    def inspected(*args: Any, **kwargs: Any) -> dict[str, Any]:
        assert kwargs["reject_invalid"] is False
        return {
            "files": 3,
            "validation": {"valid": False, "errorCount": 1},
            "validationIssues": [{"code": "INCOMPLETE_OBJECT"}],
        }

    monkeypatch.setattr("image_open_extended.run_case", inspected)
    worker_case(tmp_path / "server", source, {"id": "invalid", "expected": {}}, tmp_path, 0)
    result = json.loads((tmp_path / "001-result.json").read_text())
    assert result["openedAndEnumerated"] is True
    assert result["status"] == "failed"
    assert result["stage"] == "validation"
    assert result["validationIssues"][0]["code"] == "INCOMPLETE_OBJECT"


def test_summary_distinguishes_failures_missing_and_unrun_cases() -> None:
    rows = [
        {"status": "passed"},
        {"status": "failed"},
        {"status": "missing"},
        {"status": "not_run"},
    ]
    report = summarize(rows)
    assert report["counts"] == {"passed": 1, "failed": 1, "missing": 1, "not_run": 1}
    assert report["exitCode"] == 1
    assert report["releaseReady"] is False
    assert summarize([{"status": "missing"}])["exitCode"] == 2
