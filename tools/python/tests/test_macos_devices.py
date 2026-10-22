from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[3]


def workflow(name: str) -> dict[str, Any]:
    value = yaml.load((ROOT / ".github/workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    assert isinstance(value, dict)
    return value


def test_macos_slices_explicitly_build_device_access() -> None:
    script = (ROOT / "tools/release/build_macos_slices.sh").read_text(encoding="utf-8")
    configure = script.split("build_slice() {", 1)[1].split("cmake --build", 1)[0]
    assert "-DAXK_BUILD_DEVICES=ON" in configure
    assert "build_slice x86_64 x64-osx-axk macos-x64" in script
    assert "build_slice arm64 arm64-osx-axk macos-arm64" in script


def test_macos_only_workflow_reuses_native_jobs_without_publishing_a_partial_release() -> None:
    native = workflow("native.yml")
    standalone = workflow("native-macos.yml")
    assert set(standalone["on"]) == {"workflow_dispatch"}
    assert standalone["permissions"] == {"contents": "read"}
    assert set(standalone["jobs"]) == {"macos"}
    caller = standalone["jobs"]["macos"]
    assert caller["uses"] == native["jobs"]["macos"]["uses"] == "./.github/workflows/native-macos-platform.yml"
    assert "needs" not in caller
    assert caller["with"]["run_static_checks"] == "true"
    assert native["jobs"]["macos"]["with"]["run_static_checks"] == "false"
    assert native["jobs"]["macos"]["needs"] == "native-windows-arm64"
    assert "macos" in native["jobs"]["draft-release"]["needs"]
    inputs = standalone["on"]["workflow_dispatch"]["inputs"]
    assert inputs["debug"]["default"] == "false"
    assert inputs["notarize_macos"]["default"] == "false"


def test_shared_apple_host_queue_cannot_cancel_other_platform_work() -> None:
    native = workflow("native.yml")
    standalone = workflow("native-macos.yml")
    callers = [native["jobs"][name] for name in ("native-linux-arm64", "native-windows-arm64", "macos")]
    callers.append(standalone["jobs"]["macos"])
    queues = [caller["concurrency"] for caller in callers]
    assert all(queue == queues[0] for queue in queues)
    assert queues[0] == {"group": "native-apple-silicon-host", "cancel-in-progress": "false", "queue": "max"}
    shared = workflow("native-macos-platform.yml")
    assert shared["env"]["RUSTUP_TOOLCHAIN"] == "1.85"
    for job in shared["jobs"].values():
        assert job["runs-on"] == ["self-hosted", "macOS", "ARM64"]
        assert job["concurrency"]["group"] != queues[0]["group"]
        assert job["concurrency"]["cancel-in-progress"] == "false"
    assert shared["jobs"]["macos-universal"]["concurrency"]["group"] == "native-macos-signing"


def test_macos_checks_packaged_device_capabilities_without_accessing_hardware() -> None:
    shared = workflow("native-macos-platform.yml")
    slices = shared["jobs"]["macos-slices"]["steps"]
    rust = next(step for step in slices if step.get("uses", "").startswith("dtolnay/rust-toolchain@"))
    assert rust["with"]["toolchain"] == "1.85"
    assert rust["with"]["components"] == "rustfmt"
    checks = [step for step in slices if "run" in step]
    assert "ArchitectureCheck.cmake" in checks[0]["run"]
    static = next(step for step in slices if step.get("name") == "Validate macOS-only sources")
    assert static["if"] == "inputs.run_static_checks"
    assert "run pytest tools/python/tests" in static["run"]
    assert "contract:check format:check test check build; do" in static["run"]
    assert 'pnpm --dir apps/axkdeck "$check"' in static["run"]
    universal = shared["jobs"]["macos-universal"]["steps"]
    verify = next(step for step in universal if step.get("name") == "Verify staged universal macOS desktop DMG")
    assert "device_capability_smoke.py" in verify["run"]
    assert '--server "$sidecar"' in verify["run"]
    assert '--architecture "$architecture"' in verify["run"]
    script = (ROOT / "tools/release/build_macos_slices.sh").read_text(encoding="utf-8")
    assert "device_capability_smoke.py" in script
