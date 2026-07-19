# CLI Reference

The `axklib` executable reads, validates, extracts, creates, and alters Yamaha
A-series sampler media. Run `axklib <command> --help` for the exact option set
installed with your version.

## Exit Status

| Exit | Meaning |
| ---: | --- |
| 0 | Operation completed. |
| 1 | The request was valid, but an I/O, publication, runtime, or other operational failure prevented completion. |
| 2 | Arguments, input data, or the requested operation are invalid. |
| 3 | The command completed its scan or plan, but reported diagnostics or blocking conflicts. |

Commands write normal summaries to standard output and errors to standard
error. Commands that publish files refuse existing destinations unless
`--overwrite` is explicit. Image creation, alteration, package export, and
package import publish through a temporary sibling so a failed operation does
not leave a partial destination.

## Version Information

`axklib --version` prints the semantic API version and the independently
generated Git source identity, package basename, abbreviated commit, selected
ref, and source-tree state. Normal commands do not print this header, so their
structured and human-readable output remains unchanged. See
[Versioning And Build Identity](versioning.md) for field meanings and artifact
naming.

## Command Map

| Command | Purpose |
| --- | --- |
| `info` | Render sampler-facing partitions, groups, volumes, Programs (`PROG`), Sample Banks (`SBAC`), Samples (`SBNK`), and Wave Data (`SMPL`). |
| `extract wav` | Export selected Wave Data audio into a shared WAV pool. |
| `extract sfz` | Export WAVs and SFZ instruments for selected objects. |
| `package export` | Export a complete portable object dependency closure. |
| `package inspect` / `verify` | Inspect bounded package metadata or fully verify every payload. |
| `package plan-import` / `import` | Plan or publish a portable-package import. |
| `create manifest` | Generate an HDS, floppy, or ISO authoring starter. |
| `create hds` / `floppy` / `iso` | Create a fresh image from a versioned JSON manifest. |
| `alter manifest` | Generate an ordered HDS alteration starter. |
| `alter hds` | Plan or publish an ordered HDS alteration. |
| `validate` | Validate container allocation, directories, objects, and relationships. |
| `objects` | Write decoded object reports and optional payload files. |
| `inventory` | Write a normalized object inventory. |
| `relationships` | Write resolved sampler-object relationships. |
| `coverage` | Summarize relationship coverage and unresolved classifications. |
| `orphans` | Classify Wave Data by resolved ownership status. |
| `corpus audit` | Run inventory, validation, relationship, and bounded Wave Data checks over many inputs. |

The reporting commands write versioned JSON/CSV data plus schema sidecars to
their required output directory. See [Report Schemas](report-schemas.md) and
[CLI JSON Schemas](cli-json-schemas.md).

## Inspect And Select Content

Start with the sampler-facing tree:

```bash
axklib info source.hds
axklib info source.iso --show-quality --show-unresolved
```

Use JSON or stable selector paths for automation:

```bash
axklib info source.hds --format json
axklib info source.hds --format paths
```

`--show-default-programs` includes synthesized empty Program slots. They are
navigation aids and are not stored objects.

## Extract WAV And SFZ

Extraction syntax includes a required selection scope before the input paths:

```text
axklib extract wav SCOPE PATH... -o DIRECTORY
axklib extract sfz SCOPE PATH... -o DIRECTORY
```

Supported scopes are `file`, `volume`, `program`, `sbac`, and `sbnk`.

Export every supported object from one file:

```bash
axklib extract wav file source.iso -o exports/wav
axklib extract sfz file source.iso -o exports/sfz
```

For a narrower scope, obtain selector paths with `info --format paths`, then
repeat `--path` for each selection:

```bash
axklib extract sfz program source.hds \
  --path '<selector-from-info>' \
  -o exports/program
```

`--stereo auto` renders confirmed compatible left/right members and is the
default. `--stereo none` keeps exact mono Wave Data exports only. `--strict` stops
after the first load failure; `--progress always|never|auto` controls progress
display.

## Create Images

Generate a starter rather than writing a manifest from memory:

```bash
axklib create manifest hds -o image.json
axklib create manifest floppy -o floppy.json
axklib create manifest iso -o cdrom.json
```

Edit sampler-facing names, geometry, and audio paths, then create the image:

```bash
axklib create hds image.json -o HD00_512_generated.hds
axklib create floppy floppy.json -o authored.ima
axklib create iso cdrom.json -o authored.iso
```

The HDS starter is immediately buildable and object-empty. The floppy starter
references `tone.wav` because a generated Yamaha FAT12 image must contain at
least one object. The ISO starter is an object-empty staging volume intended to
be populated with `package import`; the standalone hardware-tested authored ISO
profile contains audio and a Sample Bank. See [Writer And
Alteration](write.md).

## Alter An HDS Image

Generate and edit a parseable starter:

```bash
axklib alter manifest -o transaction.json
```

Omit `-o` from `alter hds` to plan without publishing:

```bash
axklib alter hds source.hds transaction.json --pretty
```

Supply a different output path to apply the same checked transaction:

```bash
axklib alter hds source.hds transaction.json -o altered.hds --pretty
```

The source image is never edited in place. See [Alteration Manifest](write.md#alteration-manifest)
for operations and ordering rules.

## Portable Packages

Export one complete volume:

```bash
axklib package export source.hds \
  --partition 0 --volume 'Source Volume' \
  --root volume -o source-volume --format json
```

The output receives the root-specific suffix, such as `.axkvol`, `.axkprg`, or
`.axksbnk`. Inspecting reads bounded metadata; verifying hashes and decodes all
payloads:

```bash
axklib package inspect source-volume.axkvol --format json
axklib package verify source-volume.axkvol --format json
```

Plan an import before publishing it:

```bash
axklib package plan-import target.hds source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported","create":true}' \
  --format json
```

Apply to a separate image after reviewing conflicts and allocation:

```bash
axklib package import target.hds source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported","create":true}' \
  -o imported.hds --format json
```

Destination objects are command request fragments because they depend on the
actual package/root indexes and target media namespace. They are intentionally
not generic build-manifest templates. See [Portable Object
Packages](portable-packages.md#destination-objects) for SFS, FAT12, and ISO
destination forms and explicit rename maps.

## Validate Outputs

Validation requires an output directory:

```bash
axklib validate source.hds -o validation/source
axklib validate authored.iso -o validation/iso --policy strict
```

Policies are `normal`, `strict`, and `salvage-aware`. `--strict` is an alias for
`--policy strict`. Use a new directory or pass `--overwrite` when replacing an
existing report set.

For newly authored media, run `info` and strict validation before copying the
image to removable media or opening it in another application.

## Paths And Quoting

Quote paths and sampler names containing spaces. JSON passed directly to
`--destination` must also be protected from shell interpretation. In
PowerShell, single quotes around the JSON object are normally the simplest
form. Manifest-relative audio and source paths resolve relative to the manifest
file, not the shell working directory.

The CLI is native and does not require Python or start a scripting interpreter.
