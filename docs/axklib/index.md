# axklib

The `axklib` package is a Python library for Yamaha A3000/A4000/A5000 container and sampler-object workflows. It is also the library behind the `axklib` command-line interface.

Use this documentation in two ways:

- **Public API** pages describe the main entry points for applications and tools.
- **Diagnostic API** pages document lower-level row types and parameter decoders for callers that need detailed inspection data.

## Main Capabilities

- Open HDA/HDS, ISO9660 CD-ROM, FAT floppy, standalone object, and directory inputs.
- Represent Yamaha `FSFSDEV3SPLX` objects with source, placement, and quality metadata.
- Build current `PROG` / `SBAC` / `SBNK` / `SMPL` relationship graphs.
- Decode exact current `SMPL` waveform payloads and export WAVs.
- Validate containers, objects, relationships, exports, and sidecars with stable issue codes.

## Stability Notes

Quality labels communicate how stable a decoded value is for downstream use. `Known` values are suitable for normal reporting and export decisions. `Likely`, `Tentative`, and `Unknown` values are available for inspection and should be treated as progressively less stable, especially for generated or modified image workflows.
