# SFZ Export

`axklib extract sfz <scope>` exports WAVs first, then generates SFZ instruments
from the selected object graph in memory. This keeps the command self-contained:
users do not need to run a separate WAV extraction command.

Use `file` as the whole-input scope:

```powershell
uv run axklib extract sfz file HD00_512_example.hda -o build/exports/sfz/00001_example
```

Use narrower scopes with selectors copied from `info --format paths`:

```powershell
uv run axklib info HD00_512_example.hda --format paths
uv run axklib extract sfz program HD00_512_example.hda --path "partition_00_hd1/A3K Disk 1/Programs/001: TSUYOSHI" -o build/exports/sfz/00002_tsuyoshi
```

`--path` is repeatable for `volume`, `program`, `sbac`, and `sbnk` scopes. The
`file` scope ignores `--path` and exports every decoded volume graph from the
input set.

## Layout

Scoped WAV and SFZ exports use one shared sample pool plus one selection folder. SFZ files are written directly in the selection folder:

```text
_samples/
  physical/
  rendered/
file/
  <instrument>.sfz
program/<selector>/
  <instrument>.sfz
```

The `_samples/physical/` directory contains exact physical mono waveform
exports. The `_samples/rendered/` directory contains additive rendered stereo
WAVs when axklib has a confirmed safe stereo relationship. SFZ files reference
these shared WAVs with relative paths. Scoped export writes only the waveforms
needed by the selected file, volume, program, sample-bank group, or
sample bank; it does not materialize a hidden whole-input WAV export first. The
scoped loader also defers object payload reads and only materializes waveform
payload bytes for the selected dependency closure.

## Instrument Scope

`extract sfz` accepts `file`, `volume`, `program`, `sbac`, and `sbnk` scopes.
For non-file scopes, at least one `--path` value must be copied from
`axklib info --format paths`.

For each selected graph, axklib writes one SFZ file for each sampler-visible
sample-bank group and one SFZ file for each standalone sample bank that is not
already a member of a group.

Child sample banks covered by a `B ...` sample-bank group do not get duplicate
SFZ files. Their regions are emitted inside the group-level SFZ.

Standalone physical `SMPL` waveform objects without a decoded sampler-visible
sample-bank context are exported as WAVs and listed in the graph metadata, but
they do not become SFZ instruments on their own.

## Relative Sample Paths

Every SFZ `sample=` value is relative to the SFZ file that contains it. Paths
use forward slashes and never include absolute drive paths. For example:

```sfz
<region> pitch_keycenter=60 tune=0 sample=../../_samples/rendered/Grand L-R.wav
```

## Key Limits

Sampler key limits may use the UI value `Orig`, stored as raw `128` for the
high key limit and raw `255` for the low key limit. SFZ has no equivalent
symbolic value, so axklib resolves `Orig` to the member root key and writes
concrete `lokey` / `hikey` values. Raw key-limit bytes remain available in the
graph beside the resolved projection.

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

By default, SFZ export writes only the SFZ files plus the shared WAV pool. The
library still returns selection graphs and manifest rows to callers, and
request-level graph or manifest writing can be enabled for automated report
workflows.

## Public API

::: axklib.sfz
    options:
      members:
        - SfzExportRequest
        - SfzExportProgress
        - SfzExportManifestRow
        - SfzExportResult
        - export_sfz
