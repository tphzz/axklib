# Graph Helpers

Structured waveform export writes `volume.axklib.json` graph files. These
helpers are for applications that read those graph rows and need stable display
or navigation behavior without duplicating fallback rules.

Use `iter_volume_smpl_rows()` to read physical `SMPL` rows from either a parsed
volume graph or a `volume.axklib.json` path. For physical `SMPL` rows, exact WAV
paths remain storage-facing. Display views should use
`preferred_smpl_display_name()` for labels and keep `wav_path` as the exact file
reference.

::: axklib.graph
    options:
      members:
        - GraphInput
        - iter_volume_smpl_rows
        - preferred_smpl_display_name

