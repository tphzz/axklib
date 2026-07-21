"""Prepare content-aware GitHub Actions caches for native CMake builds."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

STATE_SCHEMA_VERSION = 1
STATE_FILENAME = ".axklib-native-cache-state.json"
NORMALIZED_MTIME_NS = 946_684_800_000_000_000
TOOLCHAIN_ENVIRONMENT = (
    "CC",
    "CXX",
    "GITHUB_WORKSPACE",
    "ImageOS",
    "ImageVersion",
    "MACOSX_DEPLOYMENT_TARGET",
    "VCToolsVersion",
    "VisualStudioVersion",
    "WindowsSDKVersion",
)


@dataclass(frozen=True)
class IndexEntry:
    mode: str
    object_id: str

    @property
    def is_regular_file(self) -> bool:
        return self.mode in {"100644", "100755"}


@dataclass(frozen=True)
class PreparationReport:
    restored: bool
    unchanged_inputs: int
    changed_inputs: int


def read_git_index(source_root: Path) -> dict[str, IndexEntry]:
    process = subprocess.run(
        ["git", "-C", str(source_root), "ls-files", "--stage", "-z"],
        check=True,
        capture_output=True,
    )
    result: dict[str, IndexEntry] = {}
    for raw_entry in process.stdout.split(b"\0"):
        if not raw_entry:
            continue
        metadata, separator, raw_path = raw_entry.partition(b"\t")
        fields = metadata.split()
        if not separator or len(fields) != 3 or fields[2] != b"0":
            raise ValueError("Git index contains an unsupported staged entry")
        path = os.fsdecode(raw_path)
        candidate = Path(path)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise ValueError("Git index contains an unsafe path")
        result[path] = IndexEntry(fields[0].decode("ascii"), fields[1].decode("ascii"))
    return result


def _serialized_entries(entries: Mapping[str, IndexEntry]) -> dict[str, dict[str, str]]:
    return {
        path: {"mode": entry.mode, "object_id": entry.object_id}
        for path, entry in sorted(entries.items())
    }


def write_state(path: Path, entries: Mapping[str, IndexEntry]) -> None:
    document = {
        "schema_version": STATE_SCHEMA_VERSION,
        "entries": _serialized_entries(entries),
    }
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_state(path: Path) -> dict[str, IndexEntry] | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            return None
        if document.get("schema_version") != STATE_SCHEMA_VERSION:
            return None
        raw_entries = document["entries"]
        if not isinstance(raw_entries, dict):
            return None
        result: dict[str, IndexEntry] = {}
        for name, raw_entry in raw_entries.items():
            if not isinstance(name, str) or not isinstance(raw_entry, dict):
                return None
            mode = raw_entry.get("mode")
            object_id = raw_entry.get("object_id")
            if not isinstance(mode, str) or not isinstance(object_id, str):
                return None
            result[name] = IndexEntry(mode, object_id)
        return result
    except (OSError, ValueError, KeyError, TypeError):
        return None


def prepare_build_cache(source_root: Path, build_directory: Path) -> PreparationReport:
    source_root = source_root.resolve()
    current = read_git_index(source_root)
    state_path = build_directory / STATE_FILENAME
    previous = read_state(state_path) if state_path.is_file() else None
    restored = previous is not None

    if build_directory.exists() and not restored:
        shutil.rmtree(build_directory)
    build_directory.mkdir(parents=True, exist_ok=True)

    unchanged = 0
    if previous is not None:
        # Fresh checkouts make every input newer than restored outputs. Normalize
        # unchanged inputs so Ninja rebuilds only paths whose Git identity changed.
        for relative_path, entry in current.items():
            if not entry.is_regular_file:
                continue
            path = source_root / relative_path
            if not path.is_file():
                raise ValueError(f"tracked build input is missing: {relative_path}")
            if previous.get(relative_path) == entry:
                os.utime(path, ns=(NORMALIZED_MTIME_NS, NORMALIZED_MTIME_NS))
                unchanged += 1
            else:
                os.utime(path, None)

    changed = len(set(current).symmetric_difference(previous or {}))
    if previous is not None:
        changed += sum(
            1 for path in set(current).intersection(previous) if current[path] != previous[path]
        )
    write_state(state_path, current)
    return PreparationReport(restored, unchanged, changed)


def _command_identity(command: Sequence[str]) -> dict[str, object]:
    executable = shutil.which(command[0])
    if executable is None:
        return {"command": list(command), "path": None, "output": "missing"}
    process = subprocess.run(command, check=False, capture_output=True, text=True)
    return {
        "command": list(command),
        "path": str(Path(executable).resolve()),
        "returncode": process.returncode,
        "output": (process.stdout + process.stderr).strip(),
    }


def toolchain_fingerprint(triplet: str, environment: Mapping[str, str] = os.environ) -> str:
    compiler = environment.get("CXX", "c++")
    compiler_command = [compiler] if Path(compiler).name.lower() in {"cl", "cl.exe"} else [compiler, "--version"]
    commands = {
        "cmake": _command_identity(["cmake", "--version"]),
        "compiler": _command_identity(compiler_command),
        "ninja": _command_identity(["ninja", "--version"]),
    }
    if platform.system() == "Darwin":
        commands["macos_sdk"] = _command_identity(["xcrun", "--show-sdk-version"])
    document = {
        "triplet": triplet,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "environment": {name: environment.get(name, "") for name in TOOLCHAIN_ENVIRONMENT},
        "commands": commands,
    }
    encoded = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def append_github_output(path: Path, values: Mapping[str, str]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        for name, value in values.items():
            stream.write(f"{name}={value}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    fingerprint = commands.add_parser("fingerprint", help="Hash the active native toolchain")
    fingerprint.add_argument("--triplet", required=True)
    fingerprint.add_argument("--github-output", type=Path, required=True)

    prepare = commands.add_parser("prepare", help="Prepare a restored incremental build tree")
    prepare.add_argument("--source-root", type=Path, required=True)
    prepare.add_argument("--build-directory", type=Path, required=True)
    prepare.add_argument("--github-output", type=Path, required=True)
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    if arguments.command == "fingerprint":
        append_github_output(
            arguments.github_output,
            {"toolchain_fingerprint": toolchain_fingerprint(arguments.triplet)},
        )
        return 0

    report = prepare_build_cache(arguments.source_root, arguments.build_directory)
    append_github_output(
        arguments.github_output,
        {
            "restored": str(report.restored).lower(),
            "unchanged_inputs": str(report.unchanged_inputs),
            "changed_inputs": str(report.changed_inputs),
        },
    )
    print(
        "native build cache: "
        f"restored={str(report.restored).lower()} "
        f"unchanged_inputs={report.unchanged_inputs} changed_inputs={report.changed_inputs}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
