"""Generate a deterministic two-member current SBNK image for export parity."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

from axklib.write import HdsImageBuilder


def _write(path: Path, samples: tuple[int, ...]) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(44_100)
        output.writeframes(b"".join(value.to_bytes(2, "little", signed=True) for value in samples))


def generate(output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    left_path = output.with_suffix(".left.wav")
    right_path = output.with_suffix(".right.wav")
    _write(left_path, (0, 1000, -1000, 2000, -2000))
    _write(right_path, (300, -300, 900, -900, 0))
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Stereo Vol")
    left = volume.add_waveform_from_wav(name="Stereo-L", path=left_path, root_key=60)
    right = volume.add_waveform_from_wav(name="Stereo-R", path=right_path, root_key=60)
    volume.add_stereo_sample_bank(
        name="Stereo Bank",
        left_waveform=left,
        right_waveform=right,
        root_key=60,
        key_low=48,
        key_high=72,
        level=96,
    )
    builder.write(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
