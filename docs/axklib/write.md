# Writer And Alteration

axklib creates fresh HDS, FAT12 floppy, and narrow ISO9660 image files from
versioned JSON manifests. It can also copy saved Yamaha object payloads into a
new floppy or ISO container. Existing-image changes use a separate ordered
transaction manifest.

```bash
axklib create hds image.json --output HD00_512_generated.hds
axklib create floppy floppy.json --output generated.ima
axklib create iso cdrom.json --output generated.iso
axklib alter hds source.hds transaction.json --output altered.hds
```

Manifest-relative input paths are resolved relative to the manifest file, not
the current working directory. Output publication is atomic. Existing output
files are refused unless `--overwrite` is supplied.

## Common Authored Content

Fresh HDS, floppy, and ISO images share the `authored_volume` schema. The
smallest useful topology is one physical waveform (`SMPL`) and one Sample Bank
(`SBNK`) that references it:

```json
"authored_volume": {
  "name": "Tone Volume",
  "waveforms": [
    {
      "id": "tone",
      "name": "Tone",
      "path": "tone.wav",
      "root_key": 60
    }
  ],
  "sample_banks": [
    {
      "name": "Tone Bank",
      "waveform_id": "tone",
      "root_key": 60,
      "key_low": 0,
      "key_high": 127,
      "level": 100
    }
  ]
}
```

Native PCM16 at a supported sampler rate preserves its exact PCM sample
sequence without resampling or requantization. The source container and byte
order still change when those samples are serialized as a Yamaha `SMPL` object.
WAV, FLAC, and AIFF input is accepted. Unsupported rates are converted with
pinned libsoxr VHQ processing; higher-precision input is converted to PCM16
with the versioned `axk-tpdf-pcg32-v1` dither policy. See
[Sampler Data Structures](sampler-data.md) for the generated object fields and
stored PCM representation.

Names written into Yamaha object and menu fields must be ASCII and at most 16
bytes. Manifest IDs such as waveform `id` are manifest-local references and do
not become sampler-facing names.

!!! warning "Current loop policy"

    Authored waveforms currently use forward loop mode over the complete
    logical waveform: start frame `0`, loop length equal to the imported frame
    count. The manifest does not yet expose one-shot mode or explicit loop
    points. Use source audio designed for a full-sample loop, or treat the
    resulting loop behavior as experimental. This limitation applies equally
    to HDS, floppy, and ISO authored output.

## Create A Hand-Authored CD-ROM ISO

Place `tone.wav` next to `cdrom.json` and write:

```json
{
  "schema_version": "1.0",
  "format": "iso9660",
  "iso": {
    "volume_id": "AXK_AUDIO",
    "raw_group": "46DEF120",
    "group_name": "AUTHORED TEST",
    "raw_volume": "F001",
    "volume_name": "TONE TEST"
  },
  "authored_volume": {
    "name": "TONE TEST",
    "waveforms": [
      {
        "id": "tone",
        "name": "Authored Tone",
        "path": "tone.wav",
        "root_key": 60
      }
    ],
    "sample_banks": [
      {
        "name": "Authored Tone",
        "waveform_id": "tone",
        "root_key": 60,
        "key_low": 60,
        "key_high": 60,
        "level": 100
      }
    ]
  }
}
```

Create and inspect the image:

```bash
axklib create iso cdrom.json --output authored.iso
axklib info authored.iso
axklib validate authored.iso --output-dir validation/iso
```

For optical media, burn `authored.iso` as a finalized, single-session disc
image. Do not copy the ISO file onto a data disc.

The exact minimal profile above has been verified on physical Yamaha A-series
hardware through group and volume enumeration, Sample Bank loading, audible
waveform playback, and pitch-correct audition. That result covers one group,
one `F001` volume, one mono `SMPL`, and one direct single-member `SBNK`. It does
not establish arbitrary group-ID generation, multiple-volume output, every
object topology, or every sampler model and firmware.

`46DEF120` is an accepted observed-form raw group identifier, not a derived
content ID. Its generation rule is unknown. The writer accepts one to eight
uppercase letters, digits, or underscores, but the hardware-verified profile
uses an eight-character uppercase hexadecimal form. Use `F001` for the one
volume emitted by the current writer; this places the group label in `F002` as
required by the Yamaha menu catalog.

The generated ISO tree and every filename are specified in
[CD-ROM Images](cdrom.md#generated-iso-file-layout).

## Create A Hand-Authored Floppy IMA

Place `tone.wav` next to `floppy.json` and write:

```json
{
  "schema_version": "1.0",
  "format": "fat12_floppy",
  "authored_volume": {
    "name": "FAT ROOT",
    "waveforms": [
      {
        "id": "tone",
        "name": "Authored Tone",
        "path": "tone.wav",
        "root_key": 60
      }
    ],
    "sample_banks": [
      {
        "name": "Authored Tone",
        "waveform_id": "tone",
        "root_key": 60,
        "key_low": 60,
        "key_high": 60,
        "level": 100
      }
    ]
  }
}
```

Create and inspect the image:

```bash
axklib create floppy floppy.json --output authored.ima
axklib info authored.ima
axklib validate authored.ima --output-dir validation/floppy
```

The output is always a deterministic 1,474,560-byte FAT12 superfloppy with two
FAT copies, a 224-entry root directory, and one DOS 8.3 root file per Yamaha
object. The `authored_volume.name` value is required by the shared schema but a
floppy has no ISO-style group or volume menu catalog; axklib displays its scope
as `FAT root`.

Host reopen and payload comparison are automated. Fresh floppy output has not
been verified on physical Yamaha hardware, so a parser-valid IMA is not yet a
hardware-compatibility guarantee. The exact FAT geometry and generated DOS 8.3
filenames are specified in
[FAT12 Floppy Images](floppy.md#generated-floppy-file-layout).

## Convert A Floppy To An ISO

This operation translates the Yamaha object-file set, not the FAT filesystem.
Given `source.ima` next to `floppy-to-iso.json`:

```json
{
  "schema_version": "1.0",
  "format": "iso9660",
  "iso": {
    "volume_id": "FLOPPY_COPY",
    "raw_group": "46DEF120",
    "group_name": "FLOPPY TEST",
    "raw_volume": "F001",
    "volume_name": "FLOPPY COPY"
  },
  "transfer": {
    "source_path": "source.ima",
    "selection": "all"
  }
}
```

Create and inspect the result:

```bash
axklib create iso floppy-to-iso.json --output floppy-copy.iso
axklib info floppy-copy.iso
axklib validate floppy-copy.iso --output-dir validation/floppy-copy
```

`selection: "all"` has this exact scope:

- The source must open as one FAT12 floppy image.
- Every file recognized as a Yamaha object must have a known type and decode
  cleanly.
- Every recognized Yamaha object payload is copied byte for byte.
- FAT cluster placement, DOS filenames, deleted entries, timestamps, volume
  labels, and non-object support files are not copied.
- ISO category directories, `0000` catalogs, `Fnnn` filenames, group label,
  and volume label are generated from the decoded object type and name plus the
  target `iso` manifest.
- The completed ISO is reopened and its complete object-payload multiset must
  equal the selected source multiset before publication.

Non-object files such as `YAMAHA.SYM` or model-specific system metadata are
therefore outside whole-source transfer. This is deliberately described as a
byte-preserving Yamaha-object transfer, not a sector-level floppy clone.

Physical Yamaha hardware has enumerated the generated group and volume for this
transfer profile. Loading and auditioning the transferred Program and its
samples remain a separate unverified step; menu visibility alone does not prove
that every transferred object type is usable from CD-ROM.

## Transfer Selected Saved Objects

Root selection copies only requested objects and their known dependency
closure. First write an object report:

```bash
axklib objects source.hds --output-dir object-report
```

Then reference one or more reported object keys:

```json
{
  "schema_version": "1.0",
  "format": "fat12_floppy",
  "transfer": {
    "source_path": "source.hds",
    "selection": "roots",
    "root_object_keys": ["<sample-bank-object-key>"]
  }
}
```

`selection` defaults to `roots` when omitted. Root selection requires a
non-empty `root_object_keys` array. Selecting an `SBNK` includes known linked
`SMPL` members. Selecting an `SBAC` or an active/source-load `PROG` assignment
continues through known `SBAC -> SBNK` and `PROG -> SBAC/SBNK` relationships.
An unresolved or ambiguous required edge is an error; the writer does not guess
a transfer closure.

## Authored Manifest Field Reference

Top-level removable-media fields:

| Field | Rule |
| --- | --- |
| `schema_version` | Required; currently exactly `"1.0"`. |
| `format` | Required; `"fat12_floppy"` or `"iso9660"`. |
| `authored_volume` / `transfer` | Exactly one is required. |
| `iso` | Required for `iso9660`; omitted for `fat12_floppy`. |

`authored_volume` fields:

| Field | Rule |
| --- | --- |
| `name` | Required non-empty string. Match `iso.volume_name` for clear ISO manifests. |
| `waveforms` | Required array. The completed authored image must contain at least one generated object. |
| `sample_banks` | Required array. |
| `sample_bank_groups` | Optional array; current groups contain 1..3 distinct Sample Bank names. |
| `programs` | Optional array; Program numbers are `1..128`. |

Waveform fields:

| Field | Rule |
| --- | --- |
| `id` | Required unique manifest-local string. |
| `name` | Required ASCII sampler object name, at most 16 bytes. |
| `path` | Required WAV, FLAC, or AIFF source path. Relative paths use the manifest directory. |
| `root_key` | Required MIDI note `0..127`. |
| `target_sample_rate` | Optional requested output rate. Omit to preserve supported native rates or use the default conversion policy. |

Direct and stereo Sample Bank fields:

| Field | Rule |
| --- | --- |
| `name` | Required unique ASCII `SBNK` name, at most 16 bytes. |
| `root_key` | Required MIDI note `0..127`. |
| `key_low`, `key_high` | Required MIDI limits `0..127`; high must not precede low. |
| `level` | Optional `0..127`; default `100`. |
| `waveform_id` | Direct left/mono member. Mutually exclusive with `interleaved_audio_path`. |
| `right_waveform_id` | Optional direct right member; it must differ from `waveform_id`. |
| `interleaved_audio_path` | Alternative two-channel source that generates linked left/right `SMPL` objects. |
| `left_waveform_name`, `right_waveform_name` | Optional names for generated interleaved members. |
| `target_sample_rate` | Optional conversion target for interleaved input. |

Direct stereo members must have equal sample rate and logical frame count.
Interleaved input is split into two physical mono objects and inherently meets
that constraint.

The current authored `SBAC`/`PROG` profile is intentionally narrow. Each group
contains 1..3 mono Sample Banks. Each Program has exactly two ordered
assignments: one distinct `sample_bank_group` on receive channel `1`, followed
by one direct `sample_bank` on receive channel `2`. Every group and direct bank
used by this profile is assigned once. Sequence (`SEQU`) and profile (`PRF3`)
payload authoring are not exposed; transfer mode can preserve existing objects
of those known types.

ISO-only fields:

| Field | Rule |
| --- | --- |
| `volume_id` | `1..32` uppercase ASCII letters, digits, or underscores. |
| `raw_group` | `1..8` uppercase ASCII letters, digits, or underscores; eight uppercase hexadecimal characters are the verified form. |
| `group_name` | Sampler-facing ASCII label, `1..16` bytes. |
| `raw_volume` | Effective writer range is `F001..F998`; the next `Fnnn` name is reserved for the group-label file. Use `F001` for the hardware-verified profile. |
| `volume_name` | Sampler-facing ASCII label, `1..16` bytes. |

## Publication And Validation Guarantees

Both removable-media writers:

1. build into a temporary sibling of the requested output;
2. reopen the temporary image through the production reader;
3. compare the complete expected and reopened object-payload multisets;
4. publish only after those checks pass.

This proves deterministic container construction and exact object retention
within axklib. It does not replace physical sampler testing for a new object
topology or profile. Keep source material until the resulting image has been
verified in the intended workflow.

Existing-image alteration performs relationship, capacity, name, and
operation-order checks before applying an ordered transaction. Application uses
a temporary destination and validates the completed image before replacement.
