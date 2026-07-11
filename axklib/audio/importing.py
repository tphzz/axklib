"""Decode and convert source audio for A-series SMPL writing."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf
import soxr

A_SERIES_SAMPLE_RATES = frozenset(
    {4_000, 5_512, 6_000, 8_000, 11_025, 12_000, 16_000, 22_050, 24_000, 32_000, 44_100, 48_000}
)
DEFAULT_SAMPLE_RATE = 44_100
_DITHER_SEED = 0x41584B


@dataclass(frozen=True)
class SamplerAudio:
    """One decoded source converted to sampler-compatible mono channel streams."""

    source_path: Path
    source_format: str
    source_subtype: str
    source_channels: int
    source_sample_rate: int
    output_sample_rate: int
    output_frames: int
    pcm_channels: tuple[bytes, ...]
    resampled: bool
    quantized: bool
    clipped_samples: int


def choose_sample_rate(source_rate: int, target_sample_rate: int | None = None) -> int:
    """Choose a documented A-series output rate for one source rate."""
    if target_sample_rate is not None:
        if isinstance(target_sample_rate, bool) or target_sample_rate not in A_SERIES_SAMPLE_RATES:
            supported = ", ".join(str(value) for value in sorted(A_SERIES_SAMPLE_RATES))
            raise ValueError(f"target sample rate must be one of: {supported}")
        return target_sample_rate
    if source_rate in A_SERIES_SAMPLE_RATES:
        return source_rate
    return DEFAULT_SAMPLE_RATE


def _pcm16_channels(data: np.ndarray) -> tuple[bytes, ...]:
    little_endian = data.astype("<i2", copy=False)
    return tuple(little_endian[:, index].tobytes() for index in range(data.shape[1]))


def _quantize_float64(data: np.ndarray, *, dither: bool) -> tuple[np.ndarray, int]:
    if not np.isfinite(data).all():
        raise ValueError("source audio contains NaN or infinite samples")
    scaled = data * 32_768.0
    clipped = int(np.count_nonzero((scaled < -32_768.0) | (scaled > 32_767.0)))
    if dither:
        rng = np.random.Generator(np.random.PCG64(_DITHER_SEED))
        scaled = scaled + rng.random(data.shape) - rng.random(data.shape)
    quantized = np.floor(scaled + 0.5)
    return np.clip(quantized, -32_768, 32_767).astype(np.int16), clipped


def import_sampler_audio(
    path: str | Path,
    *,
    expected_channels: int,
    target_sample_rate: int | None = None,
) -> SamplerAudio:
    """Decode one mono/stereo source and return signed 16-bit mono channel streams."""
    source_path = Path(path)
    if expected_channels not in {1, 2}:
        raise ValueError("expected_channels must be 1 or 2")
    try:
        with sf.SoundFile(source_path) as source:
            source_channels = int(source.channels)
            source_rate = int(source.samplerate)
            source_frames = int(source.frames)
            source_format = str(source.format)
            source_subtype = str(source.subtype)
            if source_channels > 2:
                raise ValueError(
                    f"audio source {source_path} has {source_channels} channels; "
                    "A-series import supports mono or stereo"
                )
            if source_channels != expected_channels:
                expected = "mono" if expected_channels == 1 else "stereo"
                actual = "mono" if source_channels == 1 else "stereo"
                raise ValueError(
                    f"audio source {source_path} is {actual}; this import requires {expected}"
                )
            if source_frames <= 0:
                raise ValueError(f"audio source {source_path} must contain at least one frame")
            output_rate = choose_sample_rate(source_rate, target_sample_rate)
            resampled = output_rate != source_rate
            native_pcm16 = source_subtype == "PCM_16" and not resampled
            if native_pcm16:
                pcm16 = source.read(dtype="int16", always_2d=True)
                clipped_samples = 0
            else:
                floating = source.read(dtype="float64", always_2d=True)
                if not np.isfinite(floating).all():
                    raise ValueError("source audio contains NaN or infinite samples")
                if resampled:
                    floating = soxr.resample(
                        floating,
                        source_rate,
                        output_rate,
                        quality="VHQ",
                    )
                    if floating.ndim == 1:
                        floating = floating[:, np.newaxis]
                reduces_precision = source_subtype not in {"PCM_U8", "PCM_S8", "PCM_16"}
                pcm16, clipped_samples = _quantize_float64(
                    floating,
                    dither=resampled or reduces_precision,
                )
    except sf.LibsndfileError as exc:
        raise ValueError(f"cannot decode audio source {source_path}: {exc}") from exc

    return SamplerAudio(
        source_path=source_path,
        source_format=source_format,
        source_subtype=source_subtype,
        source_channels=source_channels,
        source_sample_rate=source_rate,
        output_sample_rate=output_rate,
        output_frames=int(pcm16.shape[0]),
        pcm_channels=_pcm16_channels(pcm16),
        resampled=resampled,
        quantized=not native_pcm16,
        clipped_samples=clipped_samples,
    )


__all__ = [
    "A_SERIES_SAMPLE_RATES",
    "DEFAULT_SAMPLE_RATE",
    "SamplerAudio",
    "choose_sample_rate",
    "import_sampler_audio",
]
