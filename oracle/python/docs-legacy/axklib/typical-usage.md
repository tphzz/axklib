# Typical Usage

These examples show the intended library flow. Use `uv run axklib` for normal
command-line work; use the library when writing new maintained tools or tests.

## Open one image

```python
from pathlib import Path

import axklib

container = axklib.open(Path("HD00_512_example.hda"))
print(container.kind)
print(len(container.objects))
```

## Open many paths without losing partial success

```python
from pathlib import Path

import axklib

results = axklib.open_many([Path("disk_a.hda"), Path("disk_b.iso")])
for result in results:
    if result.container is not None:
        print(result.path, len(result.container.objects))
    else:
        print(result.path, result.error.error_code, result.error.message)
```

## Validate decoded containers

```python
from pathlib import Path

from axklib.validation import validate_paths

report = validate_paths([Path("HD00_512_example.hda")], policy="normal")
print(report.failed)
print(report.summary_counts)
```

## Build current object relationships

```python
from pathlib import Path

from axklib.relationships import build_relationship_graph_for_paths

graph = build_relationship_graph_for_paths([Path("HD00_512_example.hda")])
for edge in graph.relationships:
    print(edge.relationship_type, edge.quality, edge.source_key, edge.target_key)
```

## Decode and export waveforms

```python
from pathlib import Path

import axklib
from axklib.audio import WavExportRequest, decode_container_waveforms, export_waveforms

container = axklib.open(Path("HD00_512_example.hda"))
wave_set = decode_container_waveforms(container)
result = export_waveforms(
    WavExportRequest(
        output_dir=Path("build/exports/current-exact/00001_example"),
        waveforms=wave_set.waveforms,
        stereo_policy="auto",
        overwrite_policy="fail",
        layout="structured",
    )
)
print(len(result.written_files))
```

## Export SFZ instruments

`extract sfz file` runs WAV export first, then writes SFZ files that reference those
WAVs with relative paths. Use `file` as the whole-input scope.

```powershell
uv run axklib extract sfz file HD00_512_example.hda -o build/exports/sfz/00001_example
```

For narrower exports, copy one or more selectors from `info --format paths` and pass them to
the matching scope with repeatable `--path`. The scoped workflow writes shared samples under `_samples/`
and places SFZ files directly under the selection folder. It decodes
and writes only the selected scope plus the physical waveform dependencies
required by that scope, and waveform payload bytes are loaded lazily once the
selected dependency closure is known.

```powershell
uv run axklib info HD00_512_example.hda --format paths
uv run axklib extract sfz program HD00_512_example.hda --path "partition_00_hd1/A3K Disk 1/Programs/001: TSUYOSHI" -o build/exports/sfz/00002_tsuyoshi
```

For WAV-only whole-input extraction, use `extract wav file`:

```powershell
uv run axklib extract wav file HD00_512_example.hda -o build/exports/wav/00001_example
```

Rendered stereo WAVs are referenced when available. Otherwise the SFZ falls back
to exact physical WAV exports, including panned left/right regions when a safe
rendered stereo file was not produced.

## Read exported waveform labels

When an API workflow returns or writes selection graph rows, they use the same
row structure as older per-volume `volume.axklib.json` files. Use graph helpers
when building a display/index view so sampler-facing labels
and exact file references stay separate.

```python
from pathlib import Path

from axklib.graph import iter_volume_smpl_rows, preferred_smpl_display_name

graph_path = Path("build/exports/wav/00001_example/selection.axklib.json")  # optional graph output

for row in iter_volume_smpl_rows(graph_path):
    print(preferred_smpl_display_name(row), "->", row["wav_path"])
```

## Interpreting Quality Labels

Quality labels describe how stable a decoded value is for downstream use.
`Known` values are suitable for normal reporting and export decisions. Values
marked `Tentative` or `Unknown` are included for inspection and are not stable
inputs for generated or modified image workflows.
