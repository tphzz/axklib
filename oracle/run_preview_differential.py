"""Compare native bounded preview envelopes with exact Python PCM."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from axklib.audio import iter_waveforms
from axklib.containers import open as open_container


def _values(pcm: bytes, width: int) -> list[int]:
    if width == 1:
        return [value - 128 for value in pcm]
    return [
        int.from_bytes(pcm[offset : offset + 2], "little", signed=True)
        for offset in range(0, len(pcm), 2)
    ]


def _envelope(pcm: bytes, width: int, bins: int) -> list[list[int]]:
    values = _values(pcm, width)
    used = min(bins, len(values))
    return [
        [min(values[len(values) * index // used : len(values) * (index + 1) // used]),
         max(values[len(values) * index // used : len(values) * (index + 1) // used])]
        for index in range(used)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--bins", type=int, default=257)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    rows: list[dict[str, object]] = []
    failed = False
    for image in args.images:
        container = open_container(image)
        differences: list[str] = []
        for waveform in iter_waveforms(container):
            process = subprocess.run(
                [str(args.cpp_cli), "preview", str(image), waveform.object_key, "--bins", str(args.bins)],
                check=False,
                capture_output=True,
                text=True,
            )
            native = json.loads(process.stdout) if process.returncode == 0 else {}
            expected = _envelope(waveform.pcm, waveform.sample_width_bytes, args.bins)
            if native.get("frame_count") != waveform.frame_count or native.get("bins") != expected:
                differences.append(waveform.object_key)
        matches = not differences
        failed |= not matches
        rows.append(
            {
                "image": image.as_posix(),
                "matches": matches,
                "waveform_count": len(iter_waveforms(container)),
                "differences": differences,
            }
        )
    report = {"schema_version": "1.0", "operation": "preview-envelope", "results": rows}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
