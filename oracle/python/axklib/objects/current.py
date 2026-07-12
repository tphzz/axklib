"""Shared Yamaha A-Series object and current SMPL decoding helpers."""

from __future__ import annotations

import json
import re
import wave
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import cast

OBJECT_MAGIC = b"FSFSDEV3SPLX"
OBJECT_TYPES = {
    b"SMPL",
    b"ASPL",
    b"CSPL",
    b"ACSP",
    b"????",
    b"SBNK",
    b"SBAC",
    b"PROG",
    b"SEQU",
    b"PRF3",
}

CURRENT_SMPL_LOOP_MODE_LABELS = {
    0: "-->",
    1: "->0",
    2: "->0->",
    3: "<--",
    4: "One->",
    5: "One<-",
}


@dataclass(frozen=True)
class CurrentSmplMetadata:
    """Decoded compact metadata from a current-format SMPL object.

    Use this for current waveform extraction sidecars and parameter reports that need sample rate, root key, loop mode, loop window, link ID, and raw compact-record bytes."""

    compact_record_offset: int
    compact_record_size: int
    source_wave_name_guess: str
    smpl_group_id_0x06c: int
    smpl_link_id_0x078: int
    sample_rate_duplicate_0x07c: int
    root_key_midi_note_guess: int
    fine_tune_cents_guess: int
    wave_length_frames_0x092: int
    loop_start_frame_0x096: int
    loop_length_frames_0x09a: int
    loop_end_frame_inclusive_guess: int | None
    loop_end_frame_exclusive_guess: int | None
    loop_end_frame_a4000_ui_guess: int | None
    loop_mode_candidate_0x085: int
    loop_mode_a4000_ui_label_guess: str
    current_smpl_compact_record_hex: str


def be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def s8(value: int) -> int:
    return value if value < 0x80 else value - 0x100


def clean_ascii(data: bytes) -> str:
    return "".join(chr(byte) if 0x20 <= byte < 0x7F else "?" for byte in data.rstrip(b"\x00 "))


def clean_optional_ascii(data: bytes) -> str:
    return clean_ascii(data) if any(byte not in (0, 0x20) for byte in data) else ""


def safe_name(value: str) -> str:
    value = value.strip() or "sample"
    value = re.sub(r"[^A-Za-z0-9._ -]+", "_", value)
    value = re.sub(r"\s+", "_", value)
    return value.strip("._-") or "sample"


def swap_16bit_words(data: bytes) -> bytes:
    if len(data) % 2:
        return data
    swapped = bytearray(len(data))
    for offset in range(0, len(data), 2):
        swapped[offset] = data[offset + 1]
        swapped[offset + 1] = data[offset]
    return bytes(swapped)


def summarize_object_header(header: bytes) -> dict[str, object]:
    if len(header) < 0x42:
        raise ValueError(f"object header is too short: {len(header)} bytes")
    object_type = header[0x0C:0x10]
    summary = {
        "type": object_type.decode("ascii", errors="replace"),
        "known_type": object_type in OBJECT_TYPES,
        "header_size": be32(header, 0x10),
        "unknown_0x14": be32(header, 0x14),
        "record_size_or_header_used": be32(header, 0x18),
        "payload_bytes_0x1c": be32(header, 0x1C),
        "payload_bytes_0x20": be32(header, 0x20),
        "sample_rate_guess": be16(header, 0x28) if object_type == b"SMPL" else "",
        "bytes_per_sample_guess": be16(header, 0x2A) if object_type == b"SMPL" else "",
        "name_guess": clean_ascii(header[0x32:0x42]),
        "header_hex": header[:64].hex(),
    }
    if object_type == b"SMPL" and len(header) >= 0xAC:
        summary.update(summarize_current_smpl_compact_metadata(header))
    return summary


def decode_current_smpl_metadata(header: bytes) -> CurrentSmplMetadata:
    if len(header) < 0xAC:
        raise ValueError(f"current SMPL compact header is too short: {len(header)} bytes")
    loop_start = be32(header, 0x96)
    loop_length = be32(header, 0x9A)
    return CurrentSmplMetadata(
        compact_record_offset=0x30,
        compact_record_size=be32(header, 0x18),
        source_wave_name_guess=clean_optional_ascii(header[0x54:0x64]),
        smpl_group_id_0x06c=be32(header, 0x6C),
        smpl_link_id_0x078=be32(header, 0x78),
        sample_rate_duplicate_0x07c=be16(header, 0x7C),
        root_key_midi_note_guess=header[0x7E],
        fine_tune_cents_guess=s8(header[0x7F]),
        wave_length_frames_0x092=be32(header, 0x92),
        loop_start_frame_0x096=loop_start,
        loop_length_frames_0x09a=loop_length,
        loop_end_frame_inclusive_guess=loop_start + loop_length - 1 if loop_length else None,
        loop_end_frame_exclusive_guess=loop_start + loop_length if loop_length else None,
        loop_end_frame_a4000_ui_guess=loop_start + loop_length if loop_length else None,
        loop_mode_candidate_0x085=header[0x85],
        loop_mode_a4000_ui_label_guess=CURRENT_SMPL_LOOP_MODE_LABELS.get(header[0x85], ""),
        current_smpl_compact_record_hex=header[0x30:0xAC].hex(),
    )


def summarize_current_smpl_compact_metadata(header: bytes) -> dict[str, object]:
    return asdict(decode_current_smpl_metadata(header))


def alternating_byte_payload_ok(payload: bytes) -> bool:
    if len(payload) < 2:
        return False
    limit = len(payload) if len(payload) % 2 == 0 else len(payload) - 1
    for offset in range(1, limit, 2):
        if payload[offset] != (0x55 if offset % 4 == 1 else 0xAA):
            return False
    return True


def signed_8bit_lane_to_wav_bytes(data: bytes) -> bytes:
    return bytes((byte + 128) & 0xFF for byte in data)


def decode_current_smpl_payload_info(stored: bytes, bytes_per_sample: int) -> dict[str, object]:
    if bytes_per_sample == 2 and alternating_byte_payload_ok(stored):
        useful_lane = stored[0::2]
        return {
            "pcm": signed_8bit_lane_to_wav_bytes(useful_lane),
            "stored_payload_transform": "alternating-byte-signed-high-byte",
            "wav_sample_width_bytes": 1,
            "stored_sample_width_bytes": bytes_per_sample,
            "alternating_byte_payload_detected": True,
            "alternating_byte_useful_bytes": len(useful_lane),
        }
    if bytes_per_sample == 2:
        return {
            "pcm": swap_16bit_words(stored),
            "stored_payload_transform": "byteswap16",
            "wav_sample_width_bytes": 2,
            "stored_sample_width_bytes": bytes_per_sample,
            "alternating_byte_payload_detected": False,
            "alternating_byte_useful_bytes": "",
        }
    if bytes_per_sample == 1:
        return {
            "pcm": stored,
            "stored_payload_transform": "raw",
            "wav_sample_width_bytes": 1,
            "stored_sample_width_bytes": bytes_per_sample,
            "alternating_byte_payload_detected": False,
            "alternating_byte_useful_bytes": "",
        }
    raise ValueError(f"unsupported sample width guess: {bytes_per_sample}")


def decode_current_smpl_payload(stored: bytes, bytes_per_sample: int) -> tuple[bytes, str]:
    decoded = decode_current_smpl_payload_info(stored, bytes_per_sample)
    return cast(bytes, decoded["pcm"]), str(decoded["stored_payload_transform"])


def current_smpl_field_quality(transform: str) -> dict[str, dict[str, str]]:
    alternating_byte = transform == "alternating-byte-signed-high-byte"
    alternating_byte_artifact_note = (
        "Validated recopy rows for matched source data store ordinary current "
        "SMPL payloads instead. Treat this payload convention as a suspected third-party/conversion "
        "artifact at tentative quality; exact authorship remains unproven."
    )
    return {
        "header_size": {
            "quality": "Known",
            "basis": "direct object header field SMPL+0x010; repeated image quality",
            "notes": "Used as the payload start offset for current-format SMPL extraction.",
        },
        "stored_payload_size": {
            "quality": "Known",
            "basis": "direct object header fields SMPL+0x01c/0x020; repeated image quality",
            "notes": "Current exact PCM extraction reads this many stored payload bytes.",
        },
        "stored_payload_transform": {
            "quality": "Likely"
            if alternating_byte
            else ("Known" if transform == "byteswap16" else "Likely"),
            "basis": (
                "direct HDA alternating-byte payload quality plus exact ISO high-byte-lane matches"
                if alternating_byte
                else "null-test/image quality for current SMPL fixtures"
            ),
            "notes": (
                "Current-looking SMPL object has alternating 0x55/0xaa marker bytes in the stored payload; "
                "the useful even lane is exported as signed 8-bit converted to unsigned WAV bytes. This is "
                f"not ordinary current 16-bit PCM and remains below write-side quality. {alternating_byte_artifact_note}"
                if alternating_byte
                else (
                    "16-bit current SMPL payloads are stored big-endian and export as little-endian WAV PCM."
                    if transform == "byteswap16"
                    else "1-byte payloads are copied without byte swapping; broader signedness metadata remains open."
                )
            ),
        },
        "sample_rate": {
            "quality": "Likely",
            "basis": "direct object header field SMPL+0x028 high 16 bits; repeated image/null-test quality",
            "notes": "Used as WAV metadata. The exact PCM byte extraction does not depend on playback-rate semantics.",
        },
        "sample_width_bytes": {
            "quality": "Likely",
            "basis": (
                "alternating-byte payload detection overrides the current SMPL header width for WAV output"
                if alternating_byte
                else "direct object header field SMPL+0x02a; repeated image/null-test quality"
            ),
            "notes": (
                "For alternating-byte payloads, the current header still says 2 bytes per sample, but the emitted WAV "
                f"is 8-bit because only the useful high-byte lane is present. {alternating_byte_artifact_note}"
                if alternating_byte
                else "Used as WAV metadata and payload transform selector for current SMPL objects."
            ),
        },
        "channels": {
            "quality": "Likely",
            "basis": "current extractor behavior plus object/channel-link quality",
            "notes": "Current SMPL objects are exported as exact mono payloads; stereo is represented by linked objects.",
        },
        "name_guess": {
            "quality": "Likely",
            "basis": "direct object header bytes SMPL+0x032..0x041 and matching user-facing names",
            "notes": "Used for filenames and sidecars only; exact PCM extraction does not depend on the name.",
        },
        "sample_rate_duplicate_0x07c": {
            "quality": "Likely",
            "basis": "input consistency; repeats SMPL+0x028 high 16-bit sample-rate field in current SMPL headers",
            "notes": "Playback metadata cross-check only. Exact PCM extraction still uses the stored payload bytes.",
        },
        "root_key_midi_note_guess": {
            "quality": "Likely",
            "basis": "input consistency plus Yamaha sample-parameter range; SMPL+0x07e has MIDI-note-like range 24..111",
            "notes": "Candidate original/root key. Needs sampler validation checks before write-side use.",
        },
        "fine_tune_cents_guess": {
            "quality": "Likely",
            "basis": "input consistency plus Yamaha sample-parameter range; SMPL+0x07f is signed and observed within -38..60",
            "notes": "Candidate fine-tune cents field. Needs sampler validation checks before write-side use.",
        },
        "wave_length_frames_0x092": {
            "quality": "Likely",
            "basis": "input consistency; unaligned u32 at SMPL+0x092 tracks waveform frame count, usually frame_count-4",
            "notes": "Likely compact waveform length/end-related field. It is not used to bound PCM extraction.",
        },
        "loop_start_frame_0x096": {
            "quality": "Likely",
            "basis": "input consistency; nonzero values occur with loop-like samples and pair with SMPL+0x09a",
            "notes": "Candidate loop start frame. Needs sampler validation checks before write-side use.",
        },
        "loop_length_frames_0x09a": {
            "quality": "Likely",
            "basis": "input consistency; one-shots often match SMPL+0x092 and looping samples form plausible start+length spans",
            "notes": "Candidate loop length. Inclusive/exclusive loop end is derived in sidecars for inspection.",
        },
        "loop_end_frame_inclusive_guess": {
            "quality": "Likely",
            "basis": "derived from SMPL+0x096 loop start and SMPL+0x09a loop length",
            "notes": "Uses Yamaha manual convention that loop length is end - start + 1; needs sampler validation checks before write-side use.",
        },
        "loop_end_frame_exclusive_guess": {
            "quality": "Likely",
            "basis": "derived from SMPL+0x096 loop start and SMPL+0x09a loop length",
            "notes": "Convenience value for software APIs that use exclusive end offsets; not a stored field.",
        },
        "loop_end_frame_a4000_ui_guess": {
            "quality": "Known",
            "basis": "derived from SMPL+0x096 loop start and SMPL+0x09a loop length; matched validated A-series UI validation checks",
            "notes": "The A4000 displays loop end as start + length for checked current SMPL samples.",
        },
        "loop_mode_candidate_0x085": {
            "quality": "Likely",
            "basis": "direct image range plus validated A-series UI validation checks; observed 0 maps to -->, 1 maps to ->0, 2 maps to ->0->, 3 maps to <--, 4 maps to One->, and 5 maps to One<-",
            "notes": "All six A4000 UI loop-mode labels have sampler spot-check quality. Write-side use still needs broader save-path validation.",
        },
        "loop_mode_a4000_ui_label_guess": {
            "quality": "Likely",
            "basis": "validated A-series UI validation checks plus validated sample images for modes 3 and 5",
            "notes": "Observed mappings are 0 -->, 1 ->0, 2 ->0->, 3 <--, 4 One->, and 5 One<-.",
        },
        "smpl_group_id_0x06c": {
            "quality": "Likely",
            "basis": "input consistency and SBNK relationship reports",
            "notes": "Relationship/link grouping field used for reports, not a complete write-side semantic model.",
        },
        "smpl_link_id_0x078": {
            "quality": "Likely",
            "basis": "input consistency; SBNK current-format link fields point at this value together with sample name",
            "notes": "Not globally unique in the real HDA; exact SBNK matching must also use sample name.",
        },
        "source_wave_name_guess": {
            "quality": "Likely",
            "basis": "direct compact-header ASCII bytes SMPL+0x054..0x063 in current SMPL records",
            "notes": "Often empty; when present it appears to preserve a source/wave/instrument name.",
        },
    }


def _row_int(row: dict[str, object], key: str, default: int = 0) -> int:
    value = row.get(key, default)
    return value if isinstance(value, int) else default


def write_current_smpl_wav(
    *,
    row: dict[str, object],
    output_dir: Path,
    stem_prefix: str,
    read_stored_payload: Callable[[int, int], bytes],
    source_container: str,
    container_kind: str,
    extra_metadata: dict[str, object] | None = None,
) -> dict[str, object]:
    header_size = _row_int(row, "header_size")
    payload_size = _row_int(row, "payload_bytes_0x1c")
    sample_rate = _row_int(row, "sample_rate_guess")
    bytes_per_sample = _row_int(row, "bytes_per_sample_guess")
    name = str(row.get("name_guess", "sample"))
    stored_payload_offset_value = row.get("stored_payload_offset", header_size)
    stored_payload_offset = (
        stored_payload_offset_value if isinstance(stored_payload_offset_value, int) else header_size
    )

    stored = read_stored_payload(header_size, payload_size)
    decoded = decode_current_smpl_payload_info(stored, bytes_per_sample)
    pcm = cast(bytes, decoded["pcm"])
    transform = str(decoded["stored_payload_transform"])
    wav_sample_width_value = decoded["wav_sample_width_bytes"]
    wav_sample_width = wav_sample_width_value if isinstance(wav_sample_width_value, int) else 0
    alternating_byte_payload_detected = bool(decoded["alternating_byte_payload_detected"])

    stem = f"{stem_prefix}_{safe_name(name)}"
    wav_path = output_dir / f"{stem}.wav"
    meta_path = output_dir / f"{stem}.json"

    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(wav_sample_width)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)

    field_quality = current_smpl_field_quality(transform)
    optional_metadata_keys = (
        "sample_rate_duplicate_0x07c",
        "root_key_midi_note_guess",
        "fine_tune_cents_guess",
        "wave_length_frames_0x092",
        "loop_start_frame_0x096",
        "loop_length_frames_0x09a",
        "loop_end_frame_inclusive_guess",
        "loop_end_frame_exclusive_guess",
        "loop_end_frame_a4000_ui_guess",
        "loop_mode_candidate_0x085",
        "loop_mode_a4000_ui_label_guess",
        "smpl_group_id_0x06c",
        "smpl_link_id_0x078",
        "source_wave_name_guess",
        "compact_record_offset",
        "compact_record_size",
        "current_smpl_compact_record_hex",
    )
    for key in optional_metadata_keys:
        if key not in row:
            field_quality.pop(key, None)

    metadata = {
        "source_container": source_container,
        "container_kind": container_kind,
        "object_type": row.get("type"),
        "extraction_quality": "Likely" if alternating_byte_payload_detected else "Known",
        "extraction_basis": (
            "direct FSFSDEV3SPLXSMPL object header plus alternating-byte payload detection"
            if alternating_byte_payload_detected
            else "direct FSFSDEV3SPLXSMPL object header and stored payload bytes"
        ),
        "extraction_notes": (
            "The object header is current-format, but the stored waveform payload is alternating-byte packed; "
            "the WAV contains the useful signed high-byte lane converted to unsigned 8-bit PCM."
            if alternating_byte_payload_detected
            else "PCM byte extraction is exact for the stored payload span; WAV playback metadata carries per-field quality."
        ),
        "header_size": header_size,
        "stored_payload_offset": stored_payload_offset,
        "stored_payload_size": payload_size,
        "decoded_pcm_size": len(pcm),
        "stored_payload_transform": transform,
        "alternating_byte_payload_detected": alternating_byte_payload_detected,
        "conversion_artifact_label": (
            "suspected-third-party-or-conversion-payload-artifact"
            if alternating_byte_payload_detected
            else ""
        ),
        "conversion_artifact_quality": "Tentative" if alternating_byte_payload_detected else "",
        "conversion_artifact_notes": (
            "Only the current SMPL payload lane convention is labeled as a suspected conversion artifact. "
            "Compared current-header fields match validated ordinary current recopy rows for shared "
            "source data; exact origin remains unresolved."
            if alternating_byte_payload_detected
            else ""
        ),
        "alternating_byte_useful_bytes": decoded["alternating_byte_useful_bytes"],
        "sample_rate": sample_rate,
        "channels": 1,
        "sample_width_bytes": wav_sample_width,
        "stored_sample_width_bytes": bytes_per_sample,
        "name_guess": name,
        "field_quality": field_quality,
        "wav_path": str(wav_path),
    }
    for key in optional_metadata_keys:
        if key in row:
            metadata[key] = row[key]
    if extra_metadata:
        metadata.update(extra_metadata)
    meta_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata
