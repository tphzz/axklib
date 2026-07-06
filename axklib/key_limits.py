"""Sampler key-limit projection helpers."""

from __future__ import annotations

from collections.abc import Mapping

ORIG_KEY_LIMIT_HIGH_RAW = 128
ORIG_KEY_LIMIT_LOW_RAW = 255


def int_value(value: object) -> int | None:
    """Return an integer value, excluding booleans."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    return None


def valid_midi_key(value: int | None) -> bool:
    return value is not None and 0 <= value <= 127


def is_orig_key_limit(raw_value: int | None, *, limit: str) -> bool:
    if limit == "low":
        return raw_value == ORIG_KEY_LIMIT_LOW_RAW
    if limit == "high":
        return raw_value == ORIG_KEY_LIMIT_HIGH_RAW
    msg = f"unsupported key-limit direction: {limit}"
    raise ValueError(msg)


def resolve_key_limit(
    raw_value: int | None,
    root_key: int | None,
    *,
    limit: str,
) -> int | None:
    """Resolve a raw key-limit value to a concrete MIDI key."""
    if is_orig_key_limit(raw_value, limit=limit):
        return root_key if valid_midi_key(root_key) else None
    if valid_midi_key(raw_value):
        return raw_value
    return None


def key_limit_display(raw_value: int | None, *, limit: str) -> int | str | None:
    if is_orig_key_limit(raw_value, limit=limit):
        return "Orig"
    if valid_midi_key(raw_value):
        return raw_value
    return None


def resolve_sbnk_key_range(
    params: Mapping[str, object],
    *,
    root_key: int | None,
) -> dict[str, object] | None:
    """Resolve SBNK key-limit fields for sampler-facing/export projections."""
    low_raw = int_value(params.get("key_range_low_0x0e3"))
    high_raw = int_value(params.get("key_range_high_0x0e2"))
    if low_raw is None or high_raw is None:
        return None
    low_midi = resolve_key_limit(low_raw, root_key, limit="low")
    high_midi = resolve_key_limit(high_raw, root_key, limit="high")
    if low_midi is None or high_midi is None or high_midi < low_midi:
        return None
    basis = (
        "sampler-orig-key-limit"
        if is_orig_key_limit(low_raw, limit="low")
        or is_orig_key_limit(high_raw, limit="high")
        else "decoded-key-range"
    )
    return {
        "low_midi": low_midi,
        "high_midi": high_midi,
        "low_display": key_limit_display(low_raw, limit="low"),
        "high_display": key_limit_display(high_raw, limit="high"),
        "low_raw": low_raw,
        "high_raw": high_raw,
        "basis": basis,
    }


__all__ = [
    "ORIG_KEY_LIMIT_HIGH_RAW",
    "ORIG_KEY_LIMIT_LOW_RAW",
    "int_value",
    "is_orig_key_limit",
    "key_limit_display",
    "resolve_key_limit",
    "resolve_sbnk_key_range",
    "valid_midi_key",
]
