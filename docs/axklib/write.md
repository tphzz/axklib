# Writer And Alteration

Fresh images are described by a versioned JSON manifest. Supported geometry is
1 MiB through 2 GiB, one through eight partitions, and sampler-compatible
allocation and free-space accounting. Audio import accepts WAV, FLAC, and AIFF,
performs high-quality rate conversion when required, and splits interleaved
stereo into linked mono waveform objects.

Native PCM16 at a supported sampler rate is preserved byte for byte. Other
source widths and resampled audio are quantized with the versioned
`axk-tpdf-pcg32-v1` policy: two fixed PCG32 streams produce TPDF dither,
conversion rounds with `floor(value + 0.5)`, and values outside the PCM16 range
are counted and saturated. Import reports expose that policy name whenever it
was applied. The policy is axklib behavior, not an A-series disk-format field.

Rate conversion uses the pinned libsoxr dependency in VHQ mode. Frame counts,
channel placement, supported output rates, and repeatability are part of the
tested contract. Exact resampled PCM across different libsoxr versions is not a
compatibility promise; release builds pin the dependency graph to make produced
artifacts reproducible.

Existing-image changes use an ordered transaction manifest. Operations include
volume, waveform, sample-bank, bank-group, and Program insertion, deletion, and
rename where the referenced object state permits it. Deletion rejects unresolved
ownership and live references.

```bash
axklib create hds image.json --output HD00_512_generated.hds
axklib create floppy floppy.json --output generated.ima
axklib create iso cdrom.json --output generated.iso
axklib alter hds source.hds transaction.json --output altered.hds
```

Planning performs relationship, capacity, name, and operation-order checks.
Application uses a temporary destination and validates the completed image
before replacement. Keep an untouched source image when integrating a new write
workflow.

## Removable-media images

The removable-media manifest uses `format: "fat12_floppy"` or
`format: "iso9660"` and exactly one content mode:

- `authored_volume` uses the same waveform, sample-bank, bank-group, and Program
  data structures as an HDS volume. Object serialization and audio import are
  shared with the HDS writer.
- `transfer` copies exact saved-object bytes. Root selection follows known
  SBNK-to-SMPL, SBAC-to-SBNK, and Program assignment dependencies. An unresolved
  or ambiguous required edge is an error; the writer does not guess a closure.
  Whole-source selection accepts one FAT12 floppy and transfers every cleanly
  decoded Yamaha object without interpreting its dependency graph.

```json
{
  "schema_version": "1.0",
  "format": "iso9660",
  "iso": {
    "volume_id": "AXK_DISC",
    "raw_group": "GROUP",
    "group_name": "My Group",
    "raw_volume": "F001",
    "volume_name": "My Volume"
  },
  "authored_volume": {
    "name": "My Volume",
    "waveforms": [
      {"id": "tone", "name": "Tone", "path": "tone.wav", "root_key": 60}
    ],
    "sample_banks": [
      {
        "name": "Tone Bank",
        "waveform_id": "tone",
        "root_key": 60,
        "key_low": 0,
        "key_high": 127
      }
    ]
  }
}
```

To preserve sampler-authored object bytes, run
`axklib objects source.hds --output-dir object-report`, select roots by the
reported object keys, and use the transfer mode instead:

```json
{
  "schema_version": "1.0",
  "format": "fat12_floppy",
  "transfer": {
    "source_path": "source.hds",
    "root_object_keys": ["<sample-bank-object-key>"]
  }
}
```

Selecting a sample bank includes its known waveform members. Selecting a bank
group or active Program assignment extends that closure through the known
targets. Inactive diagnostic Program rows are not transfer dependencies.

To translate every Yamaha object from one FAT12 floppy into the selected output
container, use whole-source selection and omit `root_object_keys`:

```json
{
  "schema_version": "1.0",
  "format": "iso9660",
  "iso": {
    "volume_id": "FLOPPY_COPY",
    "raw_group": "00000010",
    "group_name": "Floppy Copy",
    "raw_volume": "F001",
    "volume_name": "Floppy Copy"
  },
  "transfer": {
    "source_path": "source.ima",
    "selection": "all"
  }
}
```

Whole-source selection rejects non-FAT12 sources, unknown object types, and
partially decoded objects. This prevents accidental flattening of multi-volume
media and prevents incomplete floppy contents from being published silently.

Both writers create a temporary image, reopen it through axklib's production
reader, compare the complete object payload multiset, and only then publish the
destination. Repeated builds from the same inputs are byte-identical.

These are deliberately narrow image-file writers, not physical-device writers.
The FAT12 profile is a fixed 1.44 MB superfloppy with two FATs, a 224-entry root
directory, DOS 8.3 object files, and no long filenames. The ISO profile emits
one primary ISO9660 tree, Yamaha group and per-category `0000` catalogs,
per-category object names starting at `F001`, and no Joliet, Rock Ridge, or
multi-extent files. The group catalog ends with the special NUL-padded
`_DSKNAME` row, which references the fixed 16-byte group label in the next
`Fnnn` file after the last volume.

Successful reopen validation proves container and object consistency within
axklib. A physical Yamaha sampler has enumerated the narrow ISO profile with a
final `_DSKNAME -> F(N+1)` group-catalog row. That result proves menu discovery
for the tested profile, not every object type, arbitrary group identifier,
floppy acceptance, or complete Program loading. Retain source material and
treat broader write combinations as experimental until separately verified.
