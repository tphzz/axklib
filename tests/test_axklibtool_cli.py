from __future__ import annotations

from pathlib import Path

import pytest

from axklib import cli as axklibtool
from axklib import content_tree as axklib_content_tree


def _put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def _put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def _write_standalone_smpl(path: Path) -> None:
    pcm_be = b"\x00\x01\x00\x02"
    payload = bytearray(0x200 + len(pcm_be))
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SMPL"
    _put_be32(payload, 0x10, 0x200)
    _put_be32(payload, 0x18, 0x7C)
    _put_be32(payload, 0x1C, len(pcm_be))
    _put_be32(payload, 0x20, len(pcm_be))
    _put_be16(payload, 0x28, 44100)
    _put_be16(payload, 0x2A, 2)
    payload[0x32 : 0x32 + 3] = b"S01"
    _put_be16(payload, 0x7C, 44100)
    payload[0x7E] = 64
    payload[0x85] = 4
    _put_be32(payload, 0x92, 2)
    _put_be32(payload, 0x96, 0)
    _put_be32(payload, 0x9A, 2)
    payload[0x200 : 0x200 + len(pcm_be)] = pcm_be
    path.write_bytes(payload)


def test_help_output_is_available(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["--help"])

    captured = capsys.readouterr()
    assert exc_info.value.code == 0
    assert "Unified Yamaha A-series" in captured.out
    assert "command" in captured.out


def test_info_reports_bad_path_without_internal_error(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    missing = tmp_path / "missing.hds"

    non_strict = axklibtool.main(["info", str(missing)])
    strict = axklibtool.main(["info", "--strict", str(missing)])

    captured = capsys.readouterr()
    assert non_strict == 1
    assert strict == 1
    assert "ERROR\tAXKLIB_CONTAINER_OPEN_FAILED" in captured.out
    assert "internal error" not in captured.err
    assert "Traceback" not in captured.err


def test_info_show_default_programs_forwards_content_tree_option(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    seen: list[bool] = []

    def fake_build_content_trees_for_paths(*args: object, **kwargs: object) -> object:
        seen.append(bool(kwargs.get("include_default_programs")))
        return axklib_content_tree.ContentTreeLoadResult(
            trees=(
                axklib_content_tree.ContentTree(
                    source_path="fixture.hds",
                    container_kind="sfs",
                    detected_format="sfs",
                    roots=(),
                ),
            ),
            load_errors=(),
        )

    monkeypatch.setattr(
        axklibtool, "build_content_trees_for_paths", fake_build_content_trees_for_paths
    )

    default_code = axklibtool.main(["info", "fixture.hds"])
    verbose_code = axklibtool.main(["info", "--show-default-programs", "fixture.hds"])

    capsys.readouterr()
    assert default_code == 0
    assert verbose_code == 0
    assert seen == [False, True]


def test_content_label_map_option_is_not_public() -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["info", "--content-label-map", "labels.json", "fixture.iso"])

    assert exc_info.value.code == 2


def test_broken_pipe_is_not_reported_as_internal_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    def raise_broken_pipe(_args: object) -> int:
        raise BrokenPipeError()

    monkeypatch.setattr(axklibtool, "run_info", raise_broken_pipe)

    code = axklibtool.main(["info", "dummy.hds"])

    captured = capsys.readouterr()
    assert code == 0
    assert "internal error" not in captured.err


def test_inventory_refuses_nonempty_output_directory(tmp_path: Path) -> None:
    output = tmp_path / "out"
    output.mkdir()
    (output / "existing.txt").write_text("keep", encoding="utf-8")

    code = axklibtool.main(["inventory", "-o", str(output), "missing.hds"])

    assert code == 1


def test_objects_reports_partial_load_failure_as_exit_code_3(tmp_path: Path) -> None:
    output = tmp_path / "objects"

    code = axklibtool.main(["objects", "-o", str(output), str(tmp_path / "missing.hds")])

    assert code == 3
    assert (output / "objects.csv").exists()


def test_objects_writes_schema_manifest(tmp_path: Path) -> None:
    output = tmp_path / "objects"

    code = axklibtool.main(["objects", "-o", str(output), str(tmp_path / "missing.hds")])

    assert code == 3
    assert (output / "_schemas" / "objects.schema.json").exists()
    assert (output / "_schemas" / "schema_index.json").exists()


def test_relationships_reports_load_failure_without_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "relationships"

    code = axklibtool.main(["relationships", "-o", str(output), str(tmp_path / "missing.hds")])

    captured = capsys.readouterr()
    assert code == 3
    assert "load_errors=1" in captured.out
    assert "internal error" not in captured.err
    assert (output / "relationships.csv").exists()
    assert (output / "load_errors.json").exists()
    assert (output / "_schemas" / "load_errors.schema.json").exists()


def test_coverage_reports_load_failure_without_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "coverage"

    code = axklibtool.main(["coverage", "-o", str(output), str(tmp_path / "missing.hds")])

    captured = capsys.readouterr()
    assert code == 3
    assert "load_errors=1" in captured.out
    assert "internal error" not in captured.err
    assert (output / "coverage_summary.json").exists()
    assert (output / "load_errors.csv").exists()


def test_extract_wav_file_fails_on_existing_targets_without_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])
    second = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])

    assert first == 0
    assert second == 1
    assert any(path.suffix == ".wav" for path in output.rglob("*.wav"))


def test_extract_wav_file_allows_existing_targets_with_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])
    second = axklibtool.main(
        ["extract", "wav", "file", "--overwrite", "-o", str(output), str(source)]
    )

    assert first == 0
    assert second == 0


def test_extract_wav_file_progress_always_reports_same_line_status(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    code = axklibtool.main(
        ["extract", "wav", "file", "--progress", "always", "-o", str(output), str(source)]
    )

    captured = capsys.readouterr()
    assert code == 0
    assert "\rloading:" in captured.err
    assert "\rresolving:" in captured.err
    assert "\rreading:" in captured.err
    assert "\rexporting:" in captured.err
    assert "_samples/physical/S01__" in captured.err
    assert captured.err.endswith("\n")


def test_extract_wav_file_writes_shared_sample_pool(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "targeted-wav"
    _write_standalone_smpl(source)

    code = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])

    assert code == 0
    wav_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.wav"))
    assert len(wav_files) == 1
    assert wav_files[0].startswith("_samples/physical/S01__")
    assert not (output / "file" / "selection.axklib.json").exists()
    assert not any(path.name.startswith(".axklib_stage_") for path in output.iterdir())


def test_extract_sfz_file_runs_wave_export_without_manifest_lists(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "sfz"
    _write_standalone_smpl(source)

    code = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])

    assert code == 0
    wav_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.wav"))
    sfz_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.sfz"))
    assert len(wav_files) == 1
    assert wav_files[0].startswith("_samples/physical/S01__")
    assert sfz_files == []
    assert not any(path.name in {"selection.axklib.json", "selection_exports.csv", "selection_exports.json", "sfz_exports.csv", "sfz_exports.json"} for path in output.rglob("*"))


def test_subcommand_help_output_is_available(capsys: pytest.CaptureFixture[str]) -> None:
    for argv in (
        ["inventory", "--help"],
        ["extract", "wav", "--help"],
        ["extract", "sfz", "--help"],
    ):
        with pytest.raises(SystemExit) as exc_info:
            axklibtool.main(list(argv))
        assert exc_info.value.code == 0

    captured = capsys.readouterr()
    help_text = captured.out.lower()
    assert "decode object inventory" in help_text
    assert "export targeted wavs" in help_text
    assert "generate sfz files" in help_text


def test_extract_waves_command_is_not_public(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["extract", "waves", "--help"])

    captured = capsys.readouterr()
    assert exc_info.value.code == 2
    assert "invalid choice" in captured.err


def test_extract_sfz_file_fails_on_existing_targets_without_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "sfz"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])
    second = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])

    assert first == 0
    assert second == 1

def test_debug_flag_controls_internal_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "validate"

    code = axklibtool.main(["validate", "-o", str(output)])
    captured = capsys.readouterr()
    assert code == 4
    assert "internal error:" in captured.err
    assert "Traceback" not in captured.err

    debug_output = tmp_path / "validate-debug"
    debug_code = axklibtool.main(["--debug", "validate", "-o", str(debug_output)])
    debug_captured = capsys.readouterr()
    assert debug_code == 4
    assert "Traceback" in debug_captured.err


def test_corpus_audit_writes_input_manifest(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "audit"
    _write_standalone_smpl(source)

    code = axklibtool.main(["corpus", "audit", "--skip-wave-smoke", "-o", str(output), str(source)])

    assert code in {0, 1}
    assert (output / "input_manifest.csv").exists()
    assert (output / "input_manifest.json").exists()
    assert (output / "_schemas" / "input_manifest.schema.json").exists()

def test_validate_fails_if_detail_report_generation_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    image = tmp_path / "source.hds"
    image.write_bytes(b"fixture")
    output = tmp_path / "validate"

    def fail_detail_reports(*_args: object, **_kwargs: object) -> list[object]:
        raise RuntimeError("detail failed")

    monkeypatch.setattr(axklibtool, "_write_validation_detail_reports", fail_detail_reports)

    code = axklibtool.main(["validate", "-o", str(output), str(image)])

    assert code == 4


def test_info_defaults_to_tree_and_summary_remains_available(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    source = tmp_path / "sample.smpl"
    _write_standalone_smpl(source)

    tree_code = axklibtool.main(["info", str(source)])
    tree_output = capsys.readouterr().out
    summary_code = axklibtool.main(["info", "--format", "summary", str(source)])
    summary_output = capsys.readouterr().out

    assert tree_code == 0
    assert "Standalone object" in tree_output
    assert "Waveforms" in tree_output
    assert summary_code == 0
    assert "objects=1" in summary_output
