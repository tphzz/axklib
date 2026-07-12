# Writer And Alteration

Fresh images are described by a versioned JSON manifest. Supported geometry is
1 MiB through 2 GiB, one through eight partitions, and sampler-compatible
allocation and free-space accounting. Audio import accepts WAV, FLAC, and AIFF,
performs high-quality rate conversion when required, and splits interleaved
stereo into linked mono waveform objects.

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
