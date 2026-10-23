from pathlib import Path
from typing import Any

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[3]
NAMES = {
    "native.yml": "00 Full - All Platforms",
    "native-windows-x64.yml": "10 Build - Windows x64",
    "native-windows-arm64.yml": "11 Build - Windows ARM64",
    "native-macos.yml": "12 Build - macOS Universal",
    "native-linux-x64.yml": "13 Build - Linux x64",
    "native-linux-arm64.yml": "14 Build - Linux ARM64",
    "publish-docs.yml": "30 Deploy - Docs",
    "native-checks.yml": "90 Helper - Checks",
    "native-platform.yml": "91 Helper - Linux/Windows",
    "native-macos-platform.yml": "92 Helper - macOS Universal",
}
RUN_NAME = "${{ github.workflow }} | ${{ github.ref_name }} | ${{ inputs.debug && 'Debug' || 'Release' }}"


def workflow(filename: str) -> dict[str, Any]:
    value = yaml.load((ROOT / ".github/workflows" / filename).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    assert isinstance(value, dict)
    return value


@pytest.mark.parametrize(("filename", "name"), NAMES.items())
def test_workflow_uses_its_canonical_navigation_name(filename: str, name: str) -> None:
    value = workflow(filename)
    assert value["name"] == name
    if name.startswith("9"):
        assert set(value["on"]) == {"workflow_call"}
        assert "run-name" not in value
    elif filename == "publish-docs.yml":
        assert set(value["on"]) == {"push", "workflow_dispatch"}
        assert "run-name" not in value
    else:
        assert set(value["on"]) == {"workflow_dispatch"}
        assert value["run-name"] == RUN_NAME
        assert value["on"]["workflow_dispatch"]["inputs"]["debug"]["type"] == "boolean"


def test_every_repository_workflow_has_a_unique_assigned_role_and_number() -> None:
    files = {path.name for path in (ROOT / ".github/workflows").iterdir() if path.suffix in {".yml", ".yaml"}}
    assert files == set(NAMES)
    names = [workflow(filename)["name"] for filename in files]
    assert len(set(names)) == len(names)
    assert len({name[:2] for name in names}) == len(names)
