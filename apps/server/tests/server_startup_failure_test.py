from __future__ import annotations

import argparse
import socket
import subprocess
import tempfile
from pathlib import Path


def run_server(server: Path, root: Path, arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(server),
            "--state-directory",
            str(root / "state"),
            "--workspace-store",
            str(root / "workspaces.json"),
            *arguments,
        ],
        capture_output=True,
        text=True,
        timeout=10.0,
        check=False,
    )


def assert_typed_startup_failure(completed: subprocess.CompletedProcess[str]) -> None:
    assert completed.returncode == 2, completed
    assert "server_start_failed" in completed.stderr, completed.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="axklib-server-startup-") as temporary:
        root = Path(temporary)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = int(listener.getsockname()[1])
            occupied = run_server(
                arguments.server,
                root / "occupied",
                [
                    "--port",
                    str(port),
                    "--token",
                    "occupied-port-token",
                    "--connection-file",
                    str(root / "occupied" / "connection.json"),
                ],
            )
        assert_typed_startup_failure(occupied)
        assert not (root / "occupied" / "connection.json").exists()

        denied = run_server(
            arguments.server,
            root / "denied",
            [
                "--bind",
                "192.0.2.1",
                "--port",
                "0",
                "--token",
                "",
                "--token-sha256",
                "test=" + "0" * 64,
                "--allow-origin",
                "https://example.invalid",
            ],
        )
        assert_typed_startup_failure(denied)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
