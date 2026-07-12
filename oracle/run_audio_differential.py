"""Compare native exact physical WAV bytes with the Python implementation."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from axklib.audio import _wav_bytes, iter_waveforms
from axklib.containers import open as open_container


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    results: list[dict[str, object]] = []
    failed = False
    with tempfile.TemporaryDirectory(prefix="axklib-native-audio-") as temporary:
        root = Path(temporary)
        for image_index, image in enumerate(args.images):
            output = root / f"image-{image_index}"
            process = subprocess.run(
                [
                    str(args.cpp_cli),
                    "extract-wav",
                    str(image),
                    "--output-dir",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            native = json.loads(process.stdout) if process.returncode == 0 else {}
            native_rows = {
                (row["partition_index"], row["sfs_id"]): row
                for row in native.get("waveforms", [])
            }
            container = open_container(image)
            objects = {
                item.object_key: item
                for item in container.objects
                if item.type == "SMPL"
            }
            differences: list[dict[str, object]] = []
            expected_keys: set[tuple[int, int]] = set()
            for waveform in iter_waveforms(container):
                item = objects[waveform.object_key]
                key = (item.partition_index, item.sfs_id)
                if not isinstance(key[0], int) or not isinstance(key[1], int):
                    continue
                normalized_key = (key[0], key[1])
                expected_keys.add(normalized_key)
                row = native_rows.get(normalized_key)
                native_bytes = (
                    Path(row["wav_path"]).read_bytes() if row is not None else b""
                )
                python_bytes = _wav_bytes(waveform)
                if native_bytes != python_bytes:
                    differences.append(
                        {
                            "partition_index": normalized_key[0],
                            "sfs_id": normalized_key[1],
                            "name": waveform.sample_name,
                            "python_sha256": _sha256(python_bytes),
                            "cpp_sha256": _sha256(native_bytes),
                        }
                    )
            extra = sorted(set(native_rows) - expected_keys)
            matches = process.returncode == 0 and not differences and not extra
            failed |= not matches
            results.append(
                {
                    "image": image.as_posix(),
                    "matches": matches,
                    "waveform_count": len(expected_keys),
                    "differences": differences,
                    "extra_native_objects": extra,
                    "cpp_stderr": process.stderr.strip(),
                }
            )
    report = {"schema_version": "1.0", "operation": "exact-wav", "results": results}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
