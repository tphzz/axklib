from __future__ import annotations

import json

from axklib.reports.schema import make_schema_manifest, write_schema_index, write_schema_manifest


def test_schema_manifest_records_columns_counts_and_quality_semantics(tmp_path) -> None:
    rows = [
        {
            "object_type": "SMPL",
            "quality": "Known",
            "code": "OK",
            "raw_offset": 12,
            "assignment_row_state": "decoded-row",
            "active_assignment_state": "unknown",
            "assignment_output1_byte_0x1d": "0xff",
            "assignment_rch_assign_gate_byte_0x28": "0x00",
            "assignment_rch_assign_display": "off",
        },
        {"object_type": "SBNK", "quality": "Likely", "code": "WARN", "raw_offset": ""},
    ]

    manifest = make_schema_manifest(
        "example",
        rows,
        source_command="axklib inventory",
        library_version="test-version",
    )

    columns = {column.name: column for column in manifest.columns}
    assert columns["quality"].semantic_notes
    assert columns["assignment_row_state"].semantic_notes
    assert columns["active_assignment_state"].semantic_notes
    assert columns["assignment_output1_byte_0x1d"].semantic_notes
    assert columns["assignment_rch_assign_gate_byte_0x28"].semantic_notes
    assert columns["assignment_rch_assign_display"].semantic_notes
    assert columns["raw_offset"].nullable
    assert manifest.quality_counts == {"Known": 1, "Likely": 1}
    assert manifest.issue_code_counts == {"OK": 1, "WARN": 1}
    assert manifest.object_type_counts == {"SBNK": 1, "SMPL": 1}
    assert manifest.quality_columns == ("quality",)
    assert manifest.issue_code_columns == ("code",)
    assert manifest.object_ref_columns == ("raw_offset",)
    assert manifest.source_command == "axklib inventory"
    assert manifest.library_version == "test-version"

    written = write_schema_manifest(
        tmp_path / "example.schema.json",
        "example",
        rows,
        source_command="axklib inventory",
        library_version="test-version",
    )
    write_schema_index(tmp_path, [written])
    index = json.loads((tmp_path / "schema_index.json").read_text(encoding="utf-8"))
    assert index["reports"][0]["report_name"] == "example"
    assert index["reports"][0]["row_count"] == 2
    assert index["reports"][0]["source_command"] == "axklib inventory"
    assert index["reports"][0]["library_version"] == "test-version"
