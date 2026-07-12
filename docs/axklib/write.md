# Writer And Alteration

Fresh images are described by a versioned JSON manifest. Supported geometry is
1 MiB through 2 GiB, one through eight partitions, and sampler-compatible
allocation and free-space accounting. Audio import accepts WAV, FLAC, and AIFF,
performs high-quality rate conversion when required, and splits interleaved
stereo into linked mono waveform objects.

Native PCM16 at a supported sampler rate is preserved byte for byte. Other
source widths and resampled audio are quantized with the versioned
`axk-tpdf-pcg32-v1` policy: two fixed PCG32 streams produce TPDF dither,
conversion rounds with `floor(value + 0.5)`, and values outside the PCM16 range
are counted and saturated. Import reports expose that policy name whenever it
was applied. The policy is axklib behavior, not an A-series disk-format field.

Rate conversion uses the pinned libsoxr dependency in VHQ mode. Frame counts,
channel placement, supported output rates, and repeatability are part of the
tested contract. Exact resampled PCM across different libsoxr versions is not a
compatibility promise; release builds pin the dependency graph to make produced
artifacts reproducible.

Existing-image changes use an ordered transaction manifest. Operations include
volume, waveform, sample-bank, bank-group, and Program insertion, deletion, and
rename where the referenced object state permits it. Deletion rejects unresolved
ownership and live references.

```bash
axklib create hds image.json --output HD00_512_generated.hds
axklib alter hds source.hds transaction.json --output altered.hds
```

Planning performs relationship, capacity, name, and operation-order checks.
Application uses a temporary destination and validates the completed image
before replacement. Keep an untouched source image when integrating a new write
workflow.
