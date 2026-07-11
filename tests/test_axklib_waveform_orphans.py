from __future__ import annotations

import json
import wave
from pathlib import Path

from axklib import cli
from axklib.alteration import alter_hds, parse_alteration_manifest
from axklib.containers import load_sfs_objects
from axklib.waveform_orphans import (
    WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED,
    WAVEFORM_STATUS_KNOWN_UNREFERENCED,
    WAVEFORM_STATUS_REFERENCED,
    analyze_hds_waveform_orphans,
)
from axklib.write import HdsImageBuilder


def _write_wav(path: Path, value: int = 1000) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(value.to_bytes(2, "little", signed=True) * 16)


def _clean_source(tmp_path: Path) -> Path:
    used_path = tmp_path / "used.wav"
    orphan_path = tmp_path / "orphan.wav"
    _write_wav(used_path, 1000)
    _write_wav(orphan_path, 2000)
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("Orphans")
    used = volume.add_waveform_from_wav(name="Used Wave", path=used_path, root_key=60)
    volume.add_waveform_from_wav(name="Orphan Wave", path=orphan_path, root_key=62)
    volume.add_sample_bank(
        name="Used Bank", waveform=used, root_key=60, key_low=0, key_high=127
    )
    output = tmp_path / "clean.hds"
    builder.write(output)
    return output


def test_orphan_checker_distinguishes_referenced_and_known_unreferenced(
    tmp_path: Path,
) -> None:
    source = _clean_source(tmp_path)

    report = analyze_hds_waveform_orphans(source)

    rows = {row.waveform_name: row for row in report.rows}
    assert rows["Used Wave"].status == WAVEFORM_STATUS_REFERENCED
    assert rows["Used Wave"].referencing_sample_banks == "Orphans/Used Bank"
    assert rows["Orphan Wave"].status == WAVEFORM_STATUS_KNOWN_UNREFERENCED
    assert rows["Orphan Wave"].volume_name == "Orphans"
    assert report.summary.referenced_count == 1
    assert report.summary.known_unreferenced_count == 1
    assert report.summary.ambiguous_or_unresolved_count == 0


def test_deleted_bank_turns_waveform_into_known_orphan(tmp_path: Path) -> None:
    source = _clean_source(tmp_path)
    transaction = parse_alteration_manifest(
        {
            "schema_version": "1.0",
            "operations": [
                {
                    "id": "delete-bank",
                    "type": "delete_sbnk",
                    "partition_index": 0,
                    "volume_name": "Orphans",
                    "sample_bank_name": "Used Bank",
                }
            ],
        }
    )
    output = tmp_path / "bank-deleted.hds"
    alter_hds(source, transaction, output_path=output)

    report = analyze_hds_waveform_orphans(output)

    assert report.summary.known_unreferenced_count == 2
    assert {row.status for row in report.rows} == {WAVEFORM_STATUS_KNOWN_UNREFERENCED}


def test_duplicate_partition_identity_remains_ambiguous(tmp_path: Path) -> None:
    wav_path = tmp_path / "duplicate.wav"
    _write_wav(wav_path)
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    for volume_name in ("First", "Second"):
        volume = partition.add_volume(volume_name)
        waveform = volume.add_waveform_from_wav(
            name="Duplicate Wave", path=wav_path, root_key=60
        )
        volume.add_sample_bank(
            name="Duplicate Bank", waveform=waveform, root_key=60, key_low=0, key_high=127
        )
    source = tmp_path / "duplicates.hds"
    builder.write(source)

    report = analyze_hds_waveform_orphans(source)

    assert report.summary.referenced_count == 0
    assert report.summary.known_unreferenced_count == 0
    assert report.summary.ambiguous_or_unresolved_count == 2
    assert {row.status for row in report.rows} == {
        WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED
    }
    assert all("matches 2 current SMPL objects" in row.notes for row in report.rows)


def test_unresolved_sbnk_member_withholds_orphan_status(tmp_path: Path) -> None:
    source = _clean_source(tmp_path)
    bank = next(item for item in load_sfs_objects(source) if item.name == "Used Bank")
    assert bank.payload_offset is not None
    with source.open("r+b") as handle:
        handle.seek(bank.payload_offset + 0xA0)
        handle.write((0xDEADBEEF).to_bytes(4, "big"))

    report = analyze_hds_waveform_orphans(source)

    assert report.summary.referenced_count == 0
    assert report.summary.known_unreferenced_count == 0
    assert report.summary.ambiguous_or_unresolved_count == 2
    assert all(
        row.status == WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED for row in report.rows
    )


def test_orphan_cli_writes_csv_json_and_summary(tmp_path: Path) -> None:
    source = _clean_source(tmp_path)
    output_dir = tmp_path / "orphan-report"

    code = cli.main(["orphans", str(source), "-o", str(output_dir)])

    assert code == 0
    rows = json.loads((output_dir / "waveform_orphans.json").read_text(encoding="utf-8"))
    summary = json.loads(
        (output_dir / "waveform_orphan_summary.json").read_text(encoding="utf-8")
    )
    assert {row["status"] for row in rows} == {
        WAVEFORM_STATUS_REFERENCED,
        WAVEFORM_STATUS_KNOWN_UNREFERENCED,
    }
    assert summary[0]["known_unreferenced_count"] == 1
    assert (output_dir / "waveform_orphans.csv").exists()
    assert (output_dir / "_schemas" / "schema_index.json").exists()
