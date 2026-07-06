# axklib

Yamaha A-series disk image and sampler object tooling.

Documentation is published at <https://tphzz.github.io/axklib/>.

## Quickstart

Install the locked environment from a checkout:

```powershell
uv sync
```

Show the CLI help:

```powershell
uv run axklib --help
```

Summarize an image, directory, or glob input:

```powershell
uv run axklib info <image-or-directory>
```

Export waveform data:

```powershell
uv run axklib extract wav file -o build/exports/wav <image-or-directory>
```

Write validation reports:

```powershell
uv run axklib validate -o build/reports/validation <image-or-directory>
```

Build the local documentation:

```powershell
uv run --group docs axklib-docs build --strict
```

## License

This project is licensed under the Mozilla Public License 2.0.

