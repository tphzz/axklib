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


def test_workflows_reuse_runner_python_without_installation_or_version_preflights() -> None:
    for path in (ROOT / ".github/workflows").glob("*.yml"):
        for job_name, job in workflow(path.name)["jobs"].items():
            for step in job.get("steps", []):
                location = (path.name, job_name, step.get("name", step.get("uses")))
                assert not step.get("uses", "").startswith("actions/setup-python@"), location
                assert "python-version" not in step.get("with", {}), location
                assert "sys.version_info" not in step.get("run", ""), location
                assert "platform.machine()" not in step.get("run", ""), location


def test_shared_native_job_selects_the_platform_python_command() -> None:
    job = workflow("native-platform.yml")["jobs"]["native"]
    assert job["env"]["AXK_PYTHON"] == (
        "${{ inputs.desktop_platform == 'windows' && 'python' || 'python3' }}"
    )
    commands = []
    for step in job["steps"]:
        for line in step.get("run", "").splitlines():
            line = line.strip()
            if "tools/python/" not in line or line.startswith("uv "):
                continue
            if step.get("shell") == "powershell":
                assert line.startswith("python tools/python/"), line
            elif line.startswith(('"$AXK_PYTHON" ', "python ", "python3 ")):
                assert line.startswith('"$AXK_PYTHON" tools/python/'), line
                commands.append(line)
    assert len(commands) >= 10


@pytest.mark.parametrize("name", ["native-checks.yml", "native.yml", "native-macos-platform.yml"])
def test_posix_workflows_use_python3_for_direct_tool_invocations(name: str) -> None:
    commands = []
    for job in workflow(name)["jobs"].values():
        for step in job.get("steps", []):
            for line in step.get("run", "").splitlines():
                line = line.strip()
                assert not line.startswith("python "), (name, step.get("name"), line)
                if line.startswith("python3 tools/python/"):
                    commands.append(line)
    assert commands


def test_macos_slice_builder_uses_python3_for_direct_tool_invocations() -> None:
    script = (ROOT / "tools/release/build_macos_slices.sh").read_text(encoding="utf-8")
    commands = [line.strip() for line in script.splitlines()]
    assert not any(line.startswith("python ") for line in commands)
    assert any(line.startswith('python3 "$source_directory/tools/python/') for line in commands)


@pytest.mark.parametrize(
    ("architecture", "runner_architecture"),
    [("x64", "X64"), ("arm64", "ARM64")],
)
def test_linux_full_and_standalone_callers_keep_the_registered_pool_labels(
    architecture: str, runner_architecture: str
) -> None:
    full = workflow("native.yml")["jobs"][f"native-linux-{architecture}"]
    standalone = workflow(f"native-linux-{architecture}.yml")["jobs"]["native"]
    labels = f'["self-hosted","Linux","{runner_architecture}","jammy","docker"]'
    for caller in (full, standalone):
        assert caller["with"]["runner_labels"] == labels


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


@pytest.mark.parametrize("target", (*TARGETS, "macos"))
def test_platform_callers_do_not_share_host_queues(target: str) -> None:
    native = workflow("native.yml")
    standalone = workflow(f"native-{target}.yml")
    caller = "macos" if target == "macos" else "native"
    full_caller = "macos" if target == "macos" else f"native-{target}"
    assert "concurrency" not in standalone["jobs"][caller]
    assert "concurrency" not in native["jobs"][full_caller]
    assert standalone["concurrency"]["group"] == (
        f"native-{target}-only-" + "${{ github.ref }}-${{ inputs.debug && 'debug' || 'release' }}"
    )
    assert standalone["concurrency"]["cancel-in-progress"] == "false"
    assert standalone["concurrency"]["queue"] == "max"


def test_reusable_checks_and_builds_keep_caller_scoped_concurrency() -> None:
    platform = workflow("native-platform.yml")
    assert "${{ github.workflow }}" in platform["jobs"]["native"]["concurrency"]["group"]
    checks = workflow("native-checks.yml")
    assert all("${{ github.workflow }}" in job["concurrency"]["group"] for job in checks["jobs"].values())


def test_full_workflow_and_release_keep_their_own_concurrency() -> None:
    native = workflow("native.yml")
    assert native["concurrency"] == {
        "group": "native-workflow-${{ github.ref }}-${{ inputs.debug && 'debug' || 'release' }}",
        "cancel-in-progress": "true",
    }
    assert native["jobs"]["draft-release"]["concurrency"] == {
        "group": "native-draft-release",
        "cancel-in-progress": "false",
    }


def test_full_release_gate_remains_transitive_over_all_targets() -> None:
    native = workflow("native.yml")
    jobs = native["jobs"]
    assert jobs["native-windows-x64"]["needs"] == "native-linux-x64"
    assert jobs["native-windows-arm64"]["needs"] == "native-linux-arm64"
    assert jobs["macos"]["needs"] == "native-windows-arm64"
    assert set(jobs["draft-release"]["needs"]) == {"native-windows-x64", "macos"}
    assert jobs["draft-release"]["if"] == "${{ github.event_name == 'workflow_dispatch' && !inputs.debug }}"


def test_release_secret_is_step_scoped_and_artifact_filenames_are_not_merged() -> None:
    native = workflow("native.yml")
    job = native["jobs"]["draft-release"]
    assert job["permissions"] == {"contents": "read", "actions": "read"}
    assert "env" not in native and "env" not in job
    release_steps = [step for step in job["steps"] if "prepare_release.py" in step.get("run", "")]
    assert len(release_steps) == 1
    assert release_steps[0]["env"]["GH_TOKEN"] == "${{ secrets.GH_TOKEN }}"
    download = next(step for step in job["steps"] if step.get("name") == "Download release deliverables")
    assert download["with"]["merge-multiple"] == "false"
    for path in (ROOT / ".github/workflows").glob("*.yml"):
        value = workflow(path.name)
        for job_name, candidate in value["jobs"].items():
            text = str(candidate)
            if path.name != "native.yml" or job_name != "draft-release":
                assert "GH_TOKEN" not in text and "prepare_release.py" not in text
        source = path.read_text(encoding="utf-8")
        assert "gh release" not in source and "git push" not in source
        assert "${{ github.token }}" not in source


def test_all_extended_queues_use_valid_non_cancelling_concurrency() -> None:
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
