# Waveform Orphan Analysis

The waveform orphan checker classifies physical current-format `SMPL` objects
without changing the source image. It is intentionally conservative so its
`known_unreferenced` rows can later serve as a deletion precondition.

Run it with:

```powershell
uv run axklib orphans .\source.hds -o .\orphan-report
```

The output directory contains:

- `waveform_orphans.csv` and `waveform_orphans.json`: one row per physical
  current `SMPL` object.
- `waveform_orphan_summary.csv` and `waveform_orphan_summary.json`: per-image
  status counts.
- `_schemas`: machine-readable report schemas.

## Statuses

`referenced`
: At least one current SBNK member resolves uniquely to the waveform by both
  physical SMPL link ID and waveform name. The row lists sampler-facing volume
  and sample-bank names under `referencing_sample_banks`.

`known_unreferenced`
: The waveform has exact visible SMPL directory placement and readable current
  metadata, every current SBNK member in the partition resolves uniquely, and
  no member references this waveform.

`ambiguous_or_unresolved`
: The checker cannot prove either state. Causes include missing directory
  placement, unreadable SMPL or SBNK metadata, a member with no unique combined
  link-ID/name match, duplicate physical identities, or an allocated record
  whose payload kind cannot be resolved. The `notes` field preserves the
  blocking basis.

A `known_unreferenced` result describes current on-disk SBNK ownership. It does
not remove the object and does not imply that similarly named or duplicate PCM
objects are interchangeable. Applications should retain the row's partition,
volume, SFS ID, link ID, and basis when presenting a future deletion.

The alteration queue's `delete_waveform` operation consumes this same
classification against the queue's current logical state. It accepts only one
exact `known_unreferenced` row; report generation itself remains read-only.

## Python API

```python
from axklib import analyze_hds_waveform_orphans

report = analyze_hds_waveform_orphans("source.hds")
for row in report.rows:
    print(row.volume_name, row.waveform_name, row.status)
```

::: axklib.waveform_orphans
