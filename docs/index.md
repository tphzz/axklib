# axklib Documentation

axklib is Yamaha A3000/A4000/A5000 disk image and sampler object tooling. It provides a Python API and the `axklib` command-line interface for inspecting supported Yamaha containers, decoding sampler objects, validating relationships, and exporting waveform data.

## Setup

Install the locked development environment from the repository root:

```powershell
uv sync
```

Show the command-line help:

```powershell
uv run axklib --help
```

## CLI Quickstart

Summarize supported inputs:

```powershell
uv run axklib info <image-or-directory>
```

Write an inventory report:

```powershell
uv run axklib inventory -o build/reports/inventory <image-or-directory>
```

Decode object rows:

```powershell
uv run axklib objects -o build/reports/objects <image-or-directory>
```

Build relationship reports:

```powershell
uv run axklib relationships -o build/reports/relationships <image-or-directory>
```

Summarize relationship coverage:

```powershell
uv run axklib coverage -o build/reports/coverage <image-or-directory>
```

Validate containers and decoded objects:

```powershell
uv run axklib validate -o build/reports/validation <image-or-directory>
```

Export waveform data:

```powershell
uv run axklib extract waves --exact --stereo auto -o build/exports/waves <image-or-directory>
```

Build the local documentation:

```powershell
uv run --group docs axklib-docs build --strict
```

Use caller-supplied ISO menu labels when a project has a separate label map:

```powershell
uv run axklib info --content-label-map labels.json <image-or-directory>
uv run axklib extract waves --content-label-map labels.json -o build/exports/waves <image-or-directory>
```

Label maps are JSON objects with synthetic source stems and raw ISO path labels:

```json
{
  "iso_group_labels": [
    {"source": "example_disc", "raw_group": "GROUP_A", "label": "GROUP_ALPHA"}
  ],
  "iso_volume_labels": [
    {
      "source": "example_disc",
      "raw_group": "GROUP_A",
      "raw_volume": "V001",
      "label": "Mapped Volume"
    }
  ]
}
```

## Documentation Sections

- [Typical Usage](axklib/typical-usage.md) shows Python API examples.
- [Public API](axklib/model.md) documents the main model and service modules.
- [Architecture](architecture.md) explains the package boundaries and data flow.

Generated HTML is written to `build/docs/site/` and is not versioned.

## Local Documentation Commands

Serve the documentation:

```powershell
uv run --group docs axklib-docs serve
```

Build the documentation strictly:

```powershell
uv run --group docs axklib-docs build --strict
```

