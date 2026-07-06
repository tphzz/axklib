# Graph Helpers

Scoped extraction builds selection graph arrays in memory and can write them on request. Older structured volume exports use `volume.axklib.json` graph objects with the same row shape.
These helpers are for applications that read those graph rows and need stable
display or navigation behavior without duplicating fallback rules.

Use `iter_volume_smpl_rows()` to read physical `SMPL` rows from a parsed graph,
a parsed selection graph array, or a path to either JSON shape. For physical
`SMPL` rows, exact WAV paths remain storage-facing. Display views should use
`preferred_smpl_display_name()` for labels and keep `wav_path` as the exact file
reference.

::: axklib.graph
    options:
      members:
        - GraphInput
        - iter_volume_smpl_rows
        - preferred_smpl_display_name
