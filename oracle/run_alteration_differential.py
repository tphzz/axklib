"""Compare native alteration output and reports with the Python oracle."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from axklib.alteration import alter_hds, load_alteration_manifest


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _json_value(value: object) -> object:
    if isinstance(value, Path):
        return value.as_posix()
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {
            field.name: _json_value(getattr(value, field.name))
            for field in dataclasses.fields(value)
        }
    if isinstance(value, tuple | list):
        return [_json_value(item) for item in value]
    return value


def run(
    cpp_cli: Path,
    cases: list[tuple[Path, Path]],
    rejection_cases: list[tuple[Path, Path]],
    report_path: Path,
) -> bool:
    results: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="axklib-alteration-oracle-") as directory:
        root = Path(directory)
        for index, (source, transaction_path) in enumerate(cases):
            python_output = root / f"{index:02d}-python.hds"
            native_output = root / f"{index:02d}-native.hds"
            transaction = load_alteration_manifest(transaction_path)
            python_result = alter_hds(source, transaction, output_path=python_output)
            process = subprocess.run(
                [
                    str(cpp_cli),
                    "alter",
                    "hds",
                    str(source),
                    str(transaction_path),
                    "--output",
                    str(native_output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            native_report = json.loads(process.stdout) if process.returncode == 0 else None
            python_operations = _json_value(python_result.operations)
            native_operations = native_report["operations"] if native_report else None
            python_hash = _digest(python_output)
            native_hash = _digest(native_output) if native_output.exists() else None
            results.append(
                {
                    "source": source.as_posix(),
                    "transaction": transaction_path.as_posix(),
                    "native_exit_code": process.returncode,
                    "native_stderr": process.stderr,
                    "python_sha256": python_hash,
                    "native_sha256": native_hash,
                    "byte_identical": python_hash == native_hash,
                    "operation_reports_identical": python_operations == native_operations,
                }
            )
        for index, (source, transaction_path) in enumerate(rejection_cases):
            python_output = root / f"reject-{index:02d}-python.hds"
            native_output = root / f"reject-{index:02d}-native.hds"
            source_hash = _digest(source)
            python_error = ""
            try:
                transaction = load_alteration_manifest(transaction_path)
                alter_hds(source, transaction, output_path=python_output)
            except (OSError, ValueError) as error:
                python_error = str(error)
            process = subprocess.run(
                [
                    str(cpp_cli),
                    "alter",
                    "hds",
                    str(source),
                    str(transaction_path),
                    "--output",
                    str(native_output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            rejected_equally = bool(python_error) and process.returncode != 0
            no_output = not python_output.exists() and not native_output.exists()
            source_preserved = _digest(source) == source_hash
            results.append(
                {
                    "source": source.as_posix(),
                    "transaction": transaction_path.as_posix(),
                    "expected_rejection": True,
                    "python_error": python_error,
                    "native_exit_code": process.returncode,
                    "native_stderr": process.stderr,
                    "rejected_equally": rejected_equally,
                    "no_output_published": no_output,
                    "source_preserved": source_preserved,
                }
            )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(
            {"schema_version": "1.0", "operation": "alteration", "results": results},
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return all(
        bool(
            item["rejected_equally"]
            and item["no_output_published"]
            and item["source_preserved"]
        )
        if item.get("expected_rejection")
        else bool(item["byte_identical"] and item["operation_reports_identical"])
        for item in results
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument(
        "--case",
        action="append",
        nargs=2,
        required=True,
        metavar=("SOURCE", "TRANSACTION"),
        type=Path,
    )
    parser.add_argument(
        "--reject-case",
        action="append",
        nargs=2,
        default=[],
        metavar=("SOURCE", "TRANSACTION"),
        type=Path,
    )
    args = parser.parse_args()
    cases = [(source, transaction) for source, transaction in args.case]
    rejection_cases = [
        (source, transaction) for source, transaction in args.reject_case
    ]
    return 0 if run(args.cpp_cli, cases, rejection_cases, args.report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
