from __future__ import annotations

from release_server_smoke import info_request_for_file


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
