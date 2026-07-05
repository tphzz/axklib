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

## Read exported waveform labels

Structured waveform export writes one `volume.axklib.json` graph per volume.
Use graph helpers when building a display/index view so sampler-facing labels
and exact file references stay separate.

```python
from pathlib import Path

from axklib.graph import iter_volume_smpl_rows, preferred_smpl_display_name

export_root = Path("build/exports/current-exact/00001_example")

for graph_path in sorted(export_root.rglob("volume.axklib.json")):
    for row in iter_volume_smpl_rows(graph_path):
        print(preferred_smpl_display_name(row), "->", row["wav_path"])
```

## Interpreting Quality Labels

Quality labels describe how stable a decoded value is for downstream use.
`Known` values are suitable for normal reporting and export decisions. Values
marked `Tentative` or `Unknown` are included for inspection and are not stable
inputs for generated or modified image workflows.

