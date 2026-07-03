"""Stereo pairing and interleaving services for exported axklib mono WAVs."""

from __future__ import annotations

import json
import re
import wave
from dataclasses import dataclass
from pathlib import Path

CHANNEL_RE = re.compile(r"^(?P<base>.*?)(?P<sep>[\s_-]+)(?P<channel>[LR])$", re.IGNORECASE)


@dataclass
class MonoSample:
    wav_path: Path
    meta_path: Path
    source_image: str
    object_offset: int
    sample_rate: int
    sample_width: int
    payload_size: int
    name: str
    pair_base: str
    channel: str


@dataclass
class StereoPair:
    left: MonoSample
    right: MonoSample
    policy: str


def safe_name(value: str) -> str:
    value = value.strip() or "sample"
    value = re.sub(r"[^A-Za-z0-9._ -]+", "_", value)
    value = re.sub(r"\s+", "_", value)
    return value.strip("._-") or "sample"


def split_channel_name(name: str) -> tuple[str, str] | None:
    match = CHANNEL_RE.match(name.strip())
    if not match:
        return None
    base = match.group("base").rstrip(" _-")
    channel = match.group("channel").upper()
    if not base:
        return None
    return base, channel


def load_sample(meta_path: Path) -> MonoSample | None:
    metadata = json.loads(meta_path.read_text(encoding="utf-8"))
    name = str(metadata.get("name_guess", ""))
    split = split_channel_name(name)
    if split is None:
        return None
    pair_base, channel = split
    wav_path = Path(str(metadata["wav_path"]))
    if not wav_path.is_absolute():
        wav_path = meta_path.parent / wav_path.name
    if not wav_path.exists():
        return None
    return MonoSample(
        wav_path=wav_path,
        meta_path=meta_path,
        source_image=str(metadata.get("source_image", "")),
        object_offset=int(metadata.get("object_offset", 0)),
        sample_rate=int(metadata["sample_rate"]),
        sample_width=int(metadata["sample_width_bytes"]),
        payload_size=int(metadata.get("decoded_pcm_size", metadata["stored_payload_size"])),
        name=name,
        pair_base=pair_base,
        channel=channel,
    )


def read_wav_pcm(path: Path) -> tuple[wave._wave_params, bytes]:
    with wave.open(str(path), "rb") as wav:
        return wav.getparams(), wav.readframes(wav.getnframes())


def pad_to_match(left: bytes, right: bytes, sample_width: int) -> tuple[bytes, bytes, int, int]:
    if len(left) % sample_width or len(right) % sample_width:
        raise ValueError("channel byte count is not aligned to sample width")
    target = max(len(left), len(right))
    left_padding = target - len(left)
    right_padding = target - len(right)
    return left + (b"\x00" * left_padding), right + (b"\x00" * right_padding), left_padding, right_padding


def trim_to_match(left: bytes, right: bytes, sample_width: int) -> tuple[bytes, bytes, int, int]:
    if len(left) % sample_width or len(right) % sample_width:
        raise ValueError("channel byte count is not aligned to sample width")
    target = min(len(left), len(right))
    left_trimmed = len(left) - target
    right_trimmed = len(right) - target
    return left[:target], right[:target], left_trimmed, right_trimmed


def interleave(left: bytes, right: bytes, sample_width: int) -> bytes:
    if len(left) % sample_width:
        raise ValueError("channel byte count is not aligned to sample width")
    if len(left) != len(right):
        raise ValueError(f"channel byte counts differ: {len(left)} != {len(right)}")
    out = bytearray(len(left) * 2)
    write_offset = 0
    for offset in range(0, len(left), sample_width):
        out[write_offset : write_offset + sample_width] = left[offset : offset + sample_width]
        write_offset += sample_width
        out[write_offset : write_offset + sample_width] = right[offset : offset + sample_width]
        write_offset += sample_width
    return bytes(out)


def pair_samples(samples: list[MonoSample], *, mismatch_policy: str) -> list[StereoPair]:
    groups: dict[tuple[str, str, int, int, int], dict[str, list[MonoSample]]] = {}
    for sample in samples:
        key = (
            sample.source_image,
            sample.pair_base,
            sample.sample_rate,
            sample.sample_width,
            sample.payload_size,
        )
        groups.setdefault(key, {"L": [], "R": []})[sample.channel].append(sample)

    pairs: list[StereoPair] = []
    used: set[Path] = set()
    for channels in groups.values():
        lefts = sorted(channels["L"], key=lambda item: item.object_offset)
        rights = sorted(channels["R"], key=lambda item: item.object_offset)
        for left, right in zip(lefts, rights, strict=False):
            pairs.append(StereoPair(left=left, right=right, policy="strict"))
            used.add(left.meta_path)
            used.add(right.meta_path)

    if mismatch_policy in {"pad-shorter", "trim-longer"}:
        loose_groups: dict[tuple[str, str, int, int], dict[str, list[MonoSample]]] = {}
        for sample in samples:
            if sample.meta_path in used:
                continue
            loose_key = (sample.source_image, sample.pair_base, sample.sample_rate, sample.sample_width)
            loose_groups.setdefault(loose_key, {"L": [], "R": []})[sample.channel].append(sample)
        for channels in loose_groups.values():
            lefts = sorted(channels["L"], key=lambda item: item.object_offset)
            rights = sorted(channels["R"], key=lambda item: item.object_offset)
            for left, right in zip(lefts, rights, strict=False):
                pairs.append(StereoPair(left=left, right=right, policy=mismatch_policy))
                used.add(left.meta_path)
                used.add(right.meta_path)
    return pairs


def write_stereo_pair(pair: StereoPair, output_dir: Path) -> dict[str, object]:
    left = pair.left
    right = pair.right
    left_params, left_pcm = read_wav_pcm(left.wav_path)
    right_params, right_pcm = read_wav_pcm(right.wav_path)
    if left_params.nchannels != 1 or right_params.nchannels != 1:
        raise ValueError("input WAVs must be mono")
    if left_params.sampwidth != right_params.sampwidth:
        raise ValueError("sample widths differ")
    if left_params.framerate != right_params.framerate:
        raise ValueError("sample rates differ")
    left_padding = 0
    right_padding = 0
    left_trimmed = 0
    right_trimmed = 0
    if left_params.nframes != right_params.nframes and pair.policy not in {"pad-shorter", "trim-longer"}:
        raise ValueError("frame counts differ")
    if pair.policy == "pad-shorter":
        left_pcm, right_pcm, left_padding, right_padding = pad_to_match(
            left_pcm,
            right_pcm,
            left_params.sampwidth,
        )
    elif pair.policy == "trim-longer":
        left_pcm, right_pcm, left_trimmed, right_trimmed = trim_to_match(
            left_pcm,
            right_pcm,
            left_params.sampwidth,
        )

    stereo_pcm = interleave(left_pcm, right_pcm, left_params.sampwidth)
    suffix_text = {"strict": "", "pad-shorter": "_padded", "trim-longer": "_trimmed"}[pair.policy]
    output_name = safe_name(f"{Path(left.source_image).stem}_{left.pair_base}{suffix_text}.wav")
    output_path = output_dir / output_name
    metadata_path = output_path.with_suffix(".json")
    suffix = 2
    while output_path.exists():
        output_path = output_dir / safe_name(f"{Path(left.source_image).stem}_{left.pair_base}_{suffix}.wav")
        metadata_path = output_path.with_suffix(".json")
        suffix += 1

    with wave.open(str(output_path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(left_params.sampwidth)
        wav.setframerate(left_params.framerate)
        wav.writeframes(stereo_pcm)

    metadata = {
        "source_image": left.source_image,
        "pair_base": left.pair_base,
        "left_wav": str(left.wav_path),
        "right_wav": str(right.wav_path),
        "left_object_offset": left.object_offset,
        "right_object_offset": right.object_offset,
        "sample_rate": left_params.framerate,
        "channels": 2,
        "sample_width_bytes": left_params.sampwidth,
        "left_input_frames": left_params.nframes,
        "right_input_frames": right_params.nframes,
        "frames": len(left_pcm) // left_params.sampwidth,
        "pairing_policy": pair.policy,
        "left_padding_bytes": left_padding,
        "right_padding_bytes": right_padding,
        "left_trimmed_bytes": left_trimmed,
        "right_trimmed_bytes": right_trimmed,
        "wav_path": str(output_path),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata

