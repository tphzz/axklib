from __future__ import annotations

import wave
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from axklib.audio.importing import (
    A_SERIES_SAMPLE_RATES,
    choose_sample_rate,
    import_sampler_audio,
)


def _write_stereo_pcm16(
    path: Path,
    left: tuple[int, ...],
    right: tuple[int, ...],
    *,
    sample_rate: int = 44_100,
) -> tuple[bytes, bytes]:
    left_pcm = b"".join(value.to_bytes(2, "little", signed=True) for value in left)
    right_pcm = b"".join(value.to_bytes(2, "little", signed=True) for value in right)
    interleaved = b"".join(
        left_pcm[offset : offset + 2] + right_pcm[offset : offset + 2]
        for offset in range(0, len(left_pcm), 2)
    )
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(interleaved)
    return left_pcm, right_pcm


def test_choose_sample_rate_preserves_documented_rates_and_defaults_to_44100() -> None:
    assert {
        choose_sample_rate(sample_rate) for sample_rate in A_SERIES_SAMPLE_RATES
    } == A_SERIES_SAMPLE_RATES
    assert choose_sample_rate(96_000) == 44_100
    assert choose_sample_rate(96_000, 22_050) == 22_050

    with pytest.raises(ValueError, match="target sample rate must be one of"):
        choose_sample_rate(44_100, 96_000)


def test_import_sampler_audio_splits_native_pcm16_exactly(tmp_path: Path) -> None:
    source = tmp_path / "stereo.wav"
    left_pcm, right_pcm = _write_stereo_pcm16(
        source,
        (0, 1000, -2000, 32767, -32768),
        (300, -400, 500, -600, 700),
    )

    imported = import_sampler_audio(source, expected_channels=2)

    assert imported.source_format == "WAV"
    assert imported.source_subtype == "PCM_16"
    assert imported.output_sample_rate == 44_100
    assert imported.output_frames == 5
    assert imported.pcm_channels == (left_pcm, right_pcm)
    assert not imported.resampled
    assert not imported.quantized
    assert imported.clipped_samples == 0


def test_import_sampler_audio_resamples_float_deterministically(tmp_path: Path) -> None:
    source = tmp_path / "float-96k.wav"
    frame_count = 960
    time = np.arange(frame_count, dtype=np.float64) / 96_000
    data = np.column_stack(
        (
            0.5 * np.sin(2 * np.pi * 1_000 * time),
            0.25 * np.sin(2 * np.pi * 2_000 * time),
        )
    )
    sf.write(source, data, 96_000, format="WAV", subtype="FLOAT")

    first = import_sampler_audio(source, expected_channels=2)
    second = import_sampler_audio(source, expected_channels=2)

    assert first.source_subtype == "FLOAT"
    assert first.output_sample_rate == 44_100
    assert abs(first.output_frames - 441) <= 1
    assert first.resampled
    assert first.quantized
    assert first.pcm_channels == second.pcm_channels
    assert first.pcm_channels[0] != first.pcm_channels[1]

    overridden = import_sampler_audio(
        source,
        expected_channels=2,
        target_sample_rate=22_050,
    )
    assert overridden.output_sample_rate == 22_050
    assert abs(overridden.output_frames - 221) <= 1


@pytest.mark.parametrize(
    ("format_name", "suffix", "subtype"),
    [("AIFF", ".aiff", "PCM_24"), ("FLAC", ".flac", "PCM_24")],
)
def test_import_sampler_audio_accepts_libsndfile_formats(
    tmp_path: Path,
    format_name: str,
    suffix: str,
    subtype: str,
) -> None:
    source = tmp_path / f"source{suffix}"
    sf.write(
        source,
        np.array([0.0, 0.25, -0.25, 0.75, -0.75], dtype=np.float64),
        32_000,
        format=format_name,
        subtype=subtype,
    )

    imported = import_sampler_audio(source, expected_channels=1)

    assert imported.source_format == format_name
    assert imported.output_sample_rate == 32_000
    assert imported.output_frames == 5
    assert imported.quantized
    assert not imported.resampled


def test_import_sampler_audio_rejects_channel_mismatch_and_surround(tmp_path: Path) -> None:
    mono = tmp_path / "mono.wav"
    surround = tmp_path / "surround.wav"
    sf.write(mono, np.zeros(8), 44_100, format="WAV", subtype="PCM_16")
    sf.write(surround, np.zeros((8, 3)), 44_100, format="WAV", subtype="PCM_16")

    with pytest.raises(ValueError, match="is mono; this import requires stereo"):
        import_sampler_audio(mono, expected_channels=2)
    with pytest.raises(ValueError, match="has 3 channels"):
        import_sampler_audio(surround, expected_channels=2)


def test_import_sampler_audio_rejects_non_finite_float_samples(tmp_path: Path) -> None:
    source = tmp_path / "non-finite.wav"
    sf.write(
        source,
        np.array([0.0, np.nan, np.inf], dtype=np.float64),
        44_100,
        format="WAV",
        subtype="DOUBLE",
    )

    with pytest.raises(ValueError, match="NaN or infinite"):
        import_sampler_audio(source, expected_channels=1)


def test_import_sampler_audio_reports_finite_clipping(tmp_path: Path) -> None:
    source = tmp_path / "clipping.wav"
    sf.write(
        source,
        np.array([-1.5, 0.0, 1.5], dtype=np.float64),
        44_100,
        format="WAV",
        subtype="FLOAT",
    )

    imported = import_sampler_audio(source, expected_channels=1)

    assert imported.clipped_samples == 2
    assert imported.pcm_channels[0][:2] == (-32_768).to_bytes(2, "little", signed=True)
    assert imported.pcm_channels[0][-2:] == (32_767).to_bytes(2, "little", signed=True)
