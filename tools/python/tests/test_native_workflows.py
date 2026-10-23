from pathlib import Path
from typing import Any

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[3]
TARGETS = ("linux-x64", "linux-arm64", "windows-x64", "windows-arm64")


def workflow(name: str) -> dict[str, Any]:
    value = yaml.load((ROOT / ".github/workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    assert isinstance(value, dict)
    return value


@pytest.mark.parametrize("target", TARGETS)
def test_standalone_target_builds_the_same_artifacts_after_shared_checks(target: str) -> None:
    standalone = workflow(f"native-{target}.yml")
    native = workflow("native.yml")
    assert set(standalone["on"]) == {"workflow_dispatch"}
    inputs = standalone["on"]["workflow_dispatch"]["inputs"]
    assert set(inputs) == {"debug"}
    assert inputs["debug"]["type"] == "boolean"
    assert inputs["debug"]["default"] == "false"
    assert standalone["permissions"] == {"contents": "read"}
    assert set(standalone["jobs"]) == {"checks", "native"}
    checks, build = standalone["jobs"]["checks"], standalone["jobs"]["native"]
    assert checks["uses"] == "./.github/workflows/native-checks.yml"
    assert checks["with"]["debug"] == "${{ inputs.debug }}"
    assert build["needs"] == "checks"
    assert build["uses"] == "./.github/workflows/native-platform.yml"
    assert build["with"] == native["jobs"][f"native-{target}"]["with"]
    assert build["with"]["artifact"] == target
    assert build["with"]["build_testing"] == "ON"
    assert build["with"]["run_tests"] == "true"
    assert build["with"]["desktop_platform"] == target.split("-", 1)[0]
    assert build["with"]["desktop_architecture"] == target.split("-", 1)[1]
    assert "contents: write" not in (ROOT / f".github/workflows/native-{target}.yml").read_text(encoding="utf-8")


def test_native_and_standalone_runs_share_complete_prerequisites() -> None:
    native = workflow("native.yml")
    assert native["jobs"]["checks"]["uses"] == "./.github/workflows/native-checks.yml"
    for target in ("linux-x64", "linux-arm64"):
        assert native["jobs"][f"native-{target}"]["needs"] == "checks"
    checks = workflow("native-checks.yml")
    assert set(checks["on"]) == {"workflow_call"}
    assert checks["permissions"] == {"contents": "read"}
    assert set(checks["jobs"]) == {"architecture", "release-tools", "desktop-static"}
    architecture = checks["jobs"]["architecture"]
    assert "needs" not in architecture
    assert any("ArchitectureCheck.cmake" in step.get("run", "") for step in architecture["steps"])
    for name in ("release-tools", "desktop-static"):
        assert checks["jobs"][name]["needs"] == "architecture"
    tools = "\n".join(step.get("run", "") for step in checks["jobs"]["release-tools"]["steps"])
    for gate in ("ruff check", "mypy", "pytest", "release-target"):
        assert gate in tools
    desktop = "\n".join(step.get("run", "") for step in checks["jobs"]["desktop-static"]["steps"])
    for gate in ("version:check", "version:test", "contract:check", "format:check", "test", "check", "build"):
        assert f"pnpm {gate}\n" in desktop + "\n"
    assert "cargo +1.85 fmt" in desktop


@pytest.mark.parametrize("target", TARGETS)
def test_host_queues_are_shared_without_cross_workflow_cancellation(target: str) -> None:
    native = workflow("native.yml")
    standalone = workflow(f"native-{target}.yml")
    queue = standalone["jobs"]["native"]["concurrency"]
    assert queue == native["jobs"][f"native-{target}"]["concurrency"]
    assert queue["group"] == ("native-apple-silicon-host" if target.endswith("arm64") else "native-x64-host")
    assert queue["cancel-in-progress"] == "false"
    assert queue["queue"] == "max"
    assert standalone["concurrency"]["cancel-in-progress"] == "false"
    assert standalone["concurrency"]["queue"] == "max"
    assert target in standalone["concurrency"]["group"]
    platform = workflow("native-platform.yml")
    assert "${{ github.workflow }}" in platform["jobs"]["native"]["concurrency"]["group"]
    checks = workflow("native-checks.yml")
    assert all("${{ github.workflow }}" in job["concurrency"]["group"] for job in checks["jobs"].values())


def test_full_release_gate_remains_transitive_over_all_targets() -> None:
    native = workflow("native.yml")
    jobs = native["jobs"]
    assert jobs["native-windows-x64"]["needs"] == "native-linux-x64"
    assert jobs["native-windows-arm64"]["needs"] == "native-linux-arm64"
    assert jobs["macos"]["needs"] == "native-windows-arm64"
    assert set(jobs["draft-release"]["needs"]) == {"native-windows-x64", "macos"}
    assert jobs["draft-release"]["if"] == "${{ github.event_name == 'workflow_dispatch' && !inputs.debug }}"


def test_all_extended_host_queues_use_valid_non_cancelling_concurrency() -> None:
    for path in (ROOT / ".github/workflows").glob("native*.yml"):
        value = workflow(path.name)
        sections = [value, *value["jobs"].values()]
        for section in sections:
            concurrency = section.get("concurrency", {})
            if "queue" in concurrency:
                # GitHub supports this key; actionlint 1.7.12 does not yet recognize it.
                assert concurrency["queue"] == "max"
                assert concurrency["cancel-in-progress"] == "false"
                assert set(concurrency) == {"group", "cancel-in-progress", "queue"}
