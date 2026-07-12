from __future__ import annotations

import hashlib
import json
from pathlib import Path


def test_sampler_authored_binary_fixtures_have_complete_manifest() -> None:
    root = Path("tests/fixtures/images/sampler-authored")
    manifest = json.loads((root / "MANIFEST.json").read_text(encoding="utf-8"))
    entries = {entry["name"]: entry for entry in manifest}
    binaries = {
        path.name
        for path in root.iterdir()
        if path.is_file() and path.suffix.lower() not in {".json", ".md"}
    }

    assert set(entries) == binaries

    required_fields = {
        "name",
        "bytes",
        "sha256",
        "purpose",
        "origin",
        "quality",
        "retention_reason",
        "expected_object_count",
        "expected_object_counts_by_type",
        "regeneration_note",
    }
    for name, entry in entries.items():
        path = root / name
        data = path.read_bytes()
        assert required_fields <= set(entry)
        assert entry["bytes"] == len(data)
        assert entry["sha256"] == hashlib.sha256(data).hexdigest()
        assert entry["purpose"]
        assert entry["origin"]
        assert entry["expected_object_count"] == sum(
            entry["expected_object_counts_by_type"].values()
        )
        assert entry["regeneration_note"]
