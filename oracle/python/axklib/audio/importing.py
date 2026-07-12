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
_DITHER_FIRST_STREAM = 0x41584B01
_DITHER_SECOND_STREAM = 0x41584B02
_PCG32_MULTIPLIER = 6_364_136_223_846_793_005
_UINT64_MASK = (1 << 64) - 1
DITHER_ALGORITHM = "axk-tpdf-pcg32-v1"


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
    dither_algorithm: str
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


class _Pcg32:
    """The fixed PCG-XSH-RR generator used by the axklib PCM16 v1 contract."""

    def __init__(self, seed: int, stream: int) -> None:
        self._state = 0
        self._increment = ((stream << 1) | 1) & _UINT64_MASK
        self.next_u32()
        self._state = (self._state + seed) & _UINT64_MASK
        self.next_u32()

    def next_u32(self) -> int:
        previous = self._state
        self._state = (previous * _PCG32_MULTIPLIER + self._increment) & _UINT64_MASK
        shifted = (((previous >> 18) ^ previous) >> 27) & 0xFFFF_FFFF
        rotation = previous >> 59
        return ((shifted >> rotation) | (shifted << ((-rotation) & 31))) & 0xFFFF_FFFF

    def unit_float64(self) -> float:
        return self.next_u32() / 4_294_967_296.0


def _dither_values(size: int, stream: int) -> np.ndarray:
    random = _Pcg32(_DITHER_SEED, stream)
    return np.fromiter((random.unit_float64() for _ in range(size)), dtype=np.float64, count=size)


def _quantize_float64(data: np.ndarray, *, dither: bool) -> tuple[np.ndarray, int]:
    if not np.isfinite(data).all():
        raise ValueError("source audio contains NaN or infinite samples")
    scaled = data * 32_768.0
    clipped = int(np.count_nonzero((scaled < -32_768.0) | (scaled > 32_767.0)))
    if dither:
        first = _dither_values(data.size, _DITHER_FIRST_STREAM).reshape(data.shape)
        second = _dither_values(data.size, _DITHER_SECOND_STREAM).reshape(data.shape)
        scaled = scaled + first - second
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
            reduces_precision = source_subtype not in {"PCM_U8", "PCM_S8", "PCM_16"}
            dither = resampled or reduces_precision
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
                pcm16, clipped_samples = _quantize_float64(
                    floating,
                    dither=dither,
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
        dither_algorithm=DITHER_ALGORITHM if dither else "",
        clipped_samples=clipped_samples,
    )


__all__ = [
    "A_SERIES_SAMPLE_RATES",
    "DEFAULT_SAMPLE_RATE",
    "DITHER_ALGORITHM",
    "SamplerAudio",
    "choose_sample_rate",
    "import_sampler_audio",
]
