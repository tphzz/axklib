from pathlib import Path
from typing import Any

import pytest

from device_capability_smoke import (
    REQUIRED_OPERATIONS,
    inspect_metadata,
    server_command,
    sidecar_arguments,
)


class MetadataClient:
    def __init__(self) -> None:
        self.requests: list[tuple[str, str]] = []
        self.operations = [{"id": name, "implemented": True} for name in REQUIRED_OPERATIONS]

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, Any]:
        self.requests.append((method, path))
        assert body is None
        if path == "/system/version":
            return 200, {"data": {"sourceIdentity": "0.6.0-pre-test"}}
        assert path == "/system/capabilities"
        return 200, {"data": {"operations": self.operations}}


def test_device_capability_check_uses_metadata_without_enumeration_or_authorization() -> None:
    client = MetadataClient()
    result = inspect_metadata(client)
    assert result["sourceIdentity"] == "0.6.0-pre-test"
    assert set(result["operations"]) == REQUIRED_OPERATIONS
    assert client.requests == [("GET", "/system/version"), ("GET", "/system/capabilities")]


@pytest.mark.parametrize("missing", sorted(REQUIRED_OPERATIONS))
def test_compiled_out_or_unbound_device_operations_fail_qualification(missing: str) -> None:
    client = MetadataClient()
    client.operations = [operation for operation in client.operations if operation["id"] != missing]
    with pytest.raises(RuntimeError, match=missing):
        inspect_metadata(client)
    client.operations.append({"id": missing, "implemented": False})
    with pytest.raises(RuntimeError, match=missing):
        inspect_metadata(client)


def test_universal_server_is_forced_to_each_requested_architecture(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("device_capability_smoke.sys.platform", "darwin")
    server = Path("/bundle/axklib-server")
    for architecture in ("x86_64", "arm64"):
        command = server_command(server, architecture)
        assert command == ["/usr/bin/arch", f"-{architecture}", str(server)]
    assert server_command(server, "native") == [str(server)]


def test_architecture_checks_cannot_silently_run_on_the_linux_host(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("device_capability_smoke.sys.platform", "linux")
    with pytest.raises(ValueError, match="macOS"):
        server_command(Path("/test/axklib-server"), "arm64")


def test_capability_smoke_uses_valid_owned_sidecar_arguments() -> None:
    state = Path("/private/state")
    arguments = sidecar_arguments(state)
    assert "--enable-devices" in arguments
    assert arguments[arguments.index("--workers") + 1] == "2"
    assert arguments[arguments.index("--parent-pid") + 1].isdigit()
    assert arguments[arguments.index("--state-directory") + 1] == str(state)
    assert arguments[arguments.index("--connection-file") + 1] == str(state / "connection.json")
    assert arguments[arguments.index("--workspace-store") + 1] == str(state / "workspaces.json")
