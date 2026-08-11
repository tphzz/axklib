from __future__ import annotations

import os
import stat
from pathlib import Path

from release_server_smoke import create_owner_only_directory, info_request_for_file


def test_create_owner_only_directory_secures_connection_file_parent(tmp_path: Path) -> None:
    state = tmp_path / "state"

    create_owner_only_directory(state)

    assert state.is_dir()
    if os.name != "nt":
        assert stat.S_IMODE(state.stat().st_mode) == stat.S_IRWXU


def test_info_request_for_file_uses_canonical_tagged_image_source() -> None:
    assert info_request_for_file("workspace", "fixture.hds") == {
        "sources": [
            {
                "kind": "FILE",
                "file": {
                    "rootId": "workspace",
                    "relativePath": "fixture.hds",
                },
            }
        ]
    }
