from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_for_file(path: Path, process: subprocess.Popen[str], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise AssertionError(f"server exited before readiness: {process.returncode}\n{stdout}\n{stderr}")
        time.sleep(0.02)
    raise AssertionError("server did not publish its connection file")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="axklib-server-parent-") as temporary:
        root = Path(temporary)
        owner = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        server = subprocess.Popen(
            [
                str(args.server),
                "--port",
                "0",
                "--state-directory",
                str(root / "state"),
                "--workspace-store",
                str(root / "workspaces.json"),
                "--connection-file",
                str(root / "connection.json"),
                "--parent-pid",
                str(owner.pid),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_file(root / "connection.json", server, 10.0)
            owner.terminate()
            owner.wait(timeout=5.0)
            started = time.monotonic()
            server.wait(timeout=2.0)
            elapsed = time.monotonic() - started
            assert server.returncode == 0, server.communicate()
            assert elapsed < 2.0, elapsed
        finally:
            if owner.poll() is None:
                owner.kill()
                owner.wait()
            if server.poll() is None:
                server.kill()
                server.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
