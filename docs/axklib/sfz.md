# SFZ Export

SFZ export uses decoded Sample ranges and exact Wave Data files. Whole-input
and selected Program exports are supported.

```bash
axklib extract sfz file source.hds --output-dir exports/sfz
axklib info source.hds --format paths
axklib extract sfz program source.hds \
  --path "partition_00/New Volume/Programs/001: Example" \
  --output-dir exports/program
```

Only exact, unambiguous relationships are rendered as regions. Unresolved rows
remain in the structured reports instead of being assigned to arbitrary Wave
Data.
