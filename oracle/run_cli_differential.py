"""Compare maintained native CLI machine contracts with the Python oracle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
from typing import Any

from axklib.write import HdsImageBuilder


def _run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=False, capture_output=True, text=True)


def _python(*arguments: str) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    package_root = Path(__file__).resolve().parent / "python"
    environment["PYTHONPATH"] = os.pathsep.join(
        [str(package_root), environment.get("PYTHONPATH", "")]
    ).rstrip(os.pathsep)
    return subprocess.run(
        [sys.executable, "-m", "axklib.cli", *arguments],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )


def _native(cli: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return _run([str(cli), *arguments])


def _json_files(root: Path, *, omit_intermediate: bool = False) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for path in sorted(root.rglob("*.json")):
        relative = path.relative_to(root)
        if omit_intermediate and relative.parts and relative.parts[0] == "_intermediate":
            continue
        values[relative.as_posix()] = json.loads(path.read_text(encoding="utf-8"))
    return values


def _file_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def _program_fixture(root: Path) -> Path:
    source = root / "tone.wav"
    with wave.open(str(source), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(44_100)
        output.writeframes(b"".join(struct.pack("<h", index * 100 - 12_000) for index in range(240)))
    builder = HdsImageBuilder(size_bytes=1024 * 1024)
    volume = builder.add_partition("New Partition").add_volume("New Volume")
    waveform = volume.add_waveform_from_wav(name="pulse 1", path=source, root_key=66)
    grouped = volume.add_sample_bank(
        name="JS01", waveform=waveform, root_key=66, key_low=0, key_high=127, level=100
    )
    direct = volume.add_sample_bank(
        name="JS02 *", waveform=waveform, root_key=66, key_low=0, key_high=127, level=100
    )
    group = volume.add_sample_bank_group(name="AUDSB", member=grouped)
    program = volume.add_program(number=1)
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct, receive_channel=2)
    image = root / "program.hds"
    builder.write(image)
    return image


def run(cpp_cli: Path, fixture: Path, report_path: Path) -> bool:
    cases: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="axklib-cli-oracle-") as directory:
        root = Path(directory)
        program = _program_fixture(root)

        for format_name in ("json", "summary", "paths", "tree"):
            python = _python("info", "--format", format_name, str(fixture))
            native = _native(cpp_cli, "info", "--format", format_name, str(fixture))
            cases.append(
                {
                    "case": f"info-{format_name}",
                    "match": python.returncode == native.returncode == 0
                    and (
                        json.loads(python.stdout) == json.loads(native.stdout)
                        if format_name == "json"
                        else python.stdout == native.stdout
                    ),
                }
            )

        for command, source in (
            ("inventory", fixture),
            ("relationships", program),
            ("orphans", fixture),
            ("validate", fixture),
        ):
            python_dir = root / f"{command}-python"
            native_dir = root / f"{command}-native"
            python = _python(command, str(source), "-o", str(python_dir))
            native = _native(cpp_cli, command, str(source), "-o", str(native_dir))
            cases.append(
                {
                    "case": command,
                    "match": python.returncode == native.returncode == 0
                    and _json_files(python_dir, omit_intermediate=True)
                    == _json_files(native_dir, omit_intermediate=True),
                }
            )

        selectors = {
            "file": "",
            "volume": "partition_00_New_Partition/New Volume",
            "program": "partition_00_New_Partition/New Volume/Programs/001: Pgm 001",
            "sbac": "partition_00_New_Partition/New Volume/Sample Banks/B AUDSB",
            "sbnk": "partition_00_New_Partition/New Volume/Sample Banks/JS02 *",
        }
        for scope, selector in selectors.items():
            python_dir = root / f"extract-{scope}-python"
            native_dir = root / f"extract-{scope}-native"
            arguments = ["extract", "sfz", scope, str(program), "-o"]
            python_args = [*arguments, str(python_dir), "--progress", "never"]
            native_args = [*arguments, str(native_dir), "--progress", "never"]
            if selector:
                python_args.extend(("--path", selector))
                native_args.extend(("--path", selector))
            python = _python(*python_args)
            native = _native(cpp_cli, *native_args)
            cases.append(
                {
                    "case": f"extract-{scope}",
                    "match": python.returncode == native.returncode == 0
                    and _file_hashes(python_dir) == _file_hashes(native_dir),
                }
            )

        transaction = root / "delete-program.json"
        transaction.write_text(
            json.dumps(
                {
                    "schema_version": "1.0",
                    "operations": [
                        {
                            "id": "delete-program",
                            "type": "delete_program",
                            "partition_index": 0,
                            "volume_name": "New Volume",
                            "program_number": 1,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        python = _python("alter", "hds", str(program), str(transaction))
        native = _native(cpp_cli, "alter", "hds", str(program), str(transaction))
        cases.append(
            {
                "case": "alter-dry-run",
                "match": python.returncode == native.returncode == 0
                and json.loads(python.stdout) == json.loads(native.stdout),
            }
        )

    success = all(bool(case["match"]) for case in cases)
    report = {
        "schema_version": "1.0",
        "operation": "maintained-cli-differential",
        "fixture": fixture.as_posix(),
        "success": success,
        "cases": cases,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return success


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    return 0 if run(args.cpp_cli, args.fixture, args.report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
