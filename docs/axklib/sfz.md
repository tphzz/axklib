# SFZ Export

`axklib extract sfz` exports structured WAVs first, then generates SFZ
instruments from the freshly written per-volume `volume.axklib.json` graphs.
This keeps the command self-contained: users do not need to run
`extract waves` separately.

```powershell
uv run axklib extract sfz HD00_512_example.hda -o build/exports/sfz/00001_example
```

For each exported volume, axklib writes:

```text
<volume>/
  SMPL/
  RENDERED/
  SFZ/
    <instrument>.sfz
  volume.axklib.json
  sfz_exports.csv
  sfz_exports.json
```

The `SMPL/` directory contains exact physical mono waveform exports. The
`RENDERED/` directory contains additive rendered stereo WAVs when axklib has a
confirmed safe stereo relationship. SFZ files prefer those rendered stereo WAVs
when available and fall back to physical WAV references when they are not. A
single physical fallback WAV is left centered; paired physical left/right
fallback regions are panned left and right.
## Instrument Scope

SFZ export is volume-scoped. axklib writes one SFZ file for each sampler-visible
sample-bank group and one SFZ file for each standalone sample bank that is not
already a member of a group.

Child sample banks covered by a `B ...` sample-bank group do not get duplicate
SFZ files. Their regions are emitted inside the group-level SFZ.

Standalone physical `SMPL` waveform objects without a decoded sampler-visible
sample-bank context are exported as WAVs and listed in `volume.axklib.json`, but
they do not become SFZ instruments on their own.

## Relative Sample Paths

Every SFZ `sample=` value is relative to the SFZ file that contains it. Paths
use forward slashes and never include absolute drive paths. For example:

```sfz
<region> pitch_keycenter=60 tune=0 sample=../RENDERED/Grand L-R.wav
```

## Key Limits

Sampler key limits may use the UI value `Orig`, stored as raw `128` for the
high key limit and raw `255` for the low key limit. SFZ has no equivalent
symbolic value, so axklib resolves `Orig` to the member root key and writes
concrete `lokey` / `hikey` values. Raw key-limit bytes remain available in
`volume.axklib.json` beside the resolved graph projection.


## Current Opcode Coverage

The first SFZ mapping is intentionally conservative. Regions may include:

- `sample`
- `lokey` and `hikey`
- `pitch_keycenter`
- `tune`
- `transpose`
- `loop_mode`, `loop_start`, and `loop_end`
- `pan` only for paired physical left/right fallback regions

Continuous `loop_end` values are converted to SFZ's inclusive endpoint. The
volume graph keeps the sampler/UI loop end value; generated SFZ subtracts one
frame or uses `loop_start + loop_length - 1` when loop length is available.

Program effects, envelopes, filters, modulation, controller mappings, output
routing, and velocity crossfade are not projected into SFZ yet. The source graph
keeps decoded fields and relationship quality so later SFZ revisions can add
more mappings without changing the WAV export contract.

## Manifests

Each volume receives `sfz_exports.csv` and `sfz_exports.json`. These files list
the generated instruments, target SFZ paths, region counts, whether rendered or
physical WAV references were used, and any skipped rows. They are the best
machine-readable summary for checking what SFZ export did for a volume.

## Public API

::: axklib.sfz
    options:
      members:
        - SfzExportRequest
        - SfzExportProgress
        - SfzExportManifestRow
        - SfzExportResult
        - export_sfz

