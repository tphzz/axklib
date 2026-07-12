"""Generate a deterministic current-object image for language parity tests."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

from axklib.write import HdsImageBuilder


def generate(output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    wav_path = output.with_suffix(".source.wav")
    samples = (0, 1000, -1000, 2000, -2000, 0)
    pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in samples)
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(pcm)

    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Object Matrix")
    waveform = volume.add_waveform_from_wav(name="Matrix Wave", path=wav_path, root_key=64)
    grouped = volume.add_sample_bank(
        name="Grouped Bank",
        waveform=waveform,
        root_key=64,
        key_low=48,
        key_high=72,
        level=93,
    )
    direct = volume.add_sample_bank(
        name="Direct Bank",
        waveform=waveform,
        root_key=64,
        key_low=48,
        key_high=72,
        level=93,
    )
    group = volume.add_sample_bank_group(name="Matrix Group", member=grouped)
    program = volume.add_program(number=33)
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct, receive_channel=2)
    builder.write(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
