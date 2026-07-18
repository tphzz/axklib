# Writer And Alteration

axklib creates fresh HDS, FAT12 floppy, and narrow ISO9660 image files from
versioned JSON manifests. It can also copy saved Yamaha object payloads into a
new floppy or ISO container. Existing-image changes use a separate ordered
transaction manifest.

```bash
axklib create hds image.json --output HD00_512_generated.hds
axklib create floppy floppy.json --output generated.ima
axklib create iso cdrom.json --output generated.iso
axklib create manifest hds --output image.json
axklib alter manifest --output transaction.json
axklib alter hds source.hds transaction.json --output altered.hds
```

Manifest-relative input paths are resolved relative to the manifest file, not
the current working directory. Output publication is atomic. Existing output
files are refused unless `--overwrite` is supplied.

## Generate A Starter Manifest

Generate a canonical starter instead of writing the schema from memory:

```bash
axklib create manifest hds --output image.json
axklib create manifest floppy --output floppy.json
axklib create manifest iso --output cdrom.json
```

The command writes formatted JSON and refuses to replace an existing manifest
unless `--overwrite` is supplied. The generated documents have deliberately
different starting content:

- `hds` is an immediately buildable 512 MiB image definition with one empty
  partition and no volumes. Add authored volume entries to the manifest or
  insert/import volumes after creation.
- `floppy` contains one waveform and Sample Bank skeleton referring to
  `tone.wav`. Replace that path and the sampler-facing names as needed. A
  Yamaha FAT12 image with no Yamaha objects is not a valid writer target, so
  the floppy starter cannot be object-empty.
- `iso` is an object-empty one-group, one-volume staging definition. It can be
  populated with `axklib package import`. Object-empty ISO output is not a
  hardware-promoted standalone disc profile; for direct audio authoring, use
  the complete example below.

The generated HDS document is:

```json
{
  "schema_version": "1.0",
  "size_bytes": 536870912,
  "partitions": [
    {
      "name": "New Partition",
      "volumes": []
    }
  ]
}
```

Create the empty image, inspect it, and then use it as an import target:

```bash
axklib create hds image.json --output HD00_512_generated.hds
axklib info HD00_512_generated.hds
axklib validate HD00_512_generated.hds --output-dir validation/hds
```

## Quick Empty HDS Profiles

Applications that need an empty import target do not have to duplicate HDS
geometry rules or synthesize a manifest. `hds_creation_profiles()` publishes
the currently admitted capacities and partition counts, and
`plan_hds_creation()` turns one of those selections into the same validated
`HdsBuildManifest` used by the regular writer:

| Profile ID | Image size | Default partitions | Available partitions |
| --- | ---: | ---: | --- |
| `floppy-scale` | 1,474,560 bytes | 1 | 1 |
| `cd-r-650` | 681,984,000 bytes | 1 | 1 through 8 |
| `cd-r-700` | 737,280,000 bytes | 1 | 1 through 8 |
| `hds-1-gib` | 1,073,741,824 bytes | 1 | 1 through 8 |
| `hds-2-gib` | 2,147,483,648 bytes | 2 | 2 through 8 |

Every partition starts without volumes. The 2 GiB profile does not offer one
partition because one SFS partition cannot represent that capacity. Callers
must use the published options instead of inferring valid partition counts
from the total byte size. Add a named volume explicitly before authoring or
importing sampler objects.

`axklib-server` exposes the same data through
`GET /api/v1/hard-disk-creation-profiles`. A client submits the chosen profile,
partition count, and sandboxed output file to
`POST /api/v1/hard-disk-build-plans`, then applies the returned plan token with
the regular image-build operation. Planning reserves and validates the output;
publication remains atomic. The HTTP contract expresses profile IDs as wire
enums such as `FLOPPY_SCALE` and `CD_R_700`.

The floppy-scale and CD-R-scale choices are still HDS containers. They are
useful small or removable-media-sized workspaces that can later receive
packages and be converted through a supported transfer workflow. They are not
empty FAT12 floppy images or ISO9660 disc images. Those media require Yamaha
catalog/object content, so axklib does not advertise unsupported empty-media
profiles.

Generate an alteration starter separately:

```bash
axklib alter manifest --output transaction.json
```

Creation and alteration use different schemas. A creation manifest describes a
complete new container; an alteration manifest is an ordered transaction
against an existing HDS image.

## Common Authored Content

Fresh HDS, floppy, and ISO images share the same authored volume fields. HDS
places those fields inside each `partitions[].volumes[]` entry; removable-media
manifests place them in `authored_volume`. The smallest useful topology is one
Wave Data object (`SMPL`) and one Sample (`SBNK`) that references it:

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

    Authored Wave Data entries currently use forward loop mode over the complete
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
one `F001` volume, one mono Wave Data object (`SMPL`), and one direct
single-member Sample (`SBNK`). It does
not establish arbitrary group-ID generation, multiple-volume output, every
object topology, or every sampler model and system version.

An adjacent fresh profile is also hardware-verified with one Program containing
both assignment forms supported by the writer: one Sample Bank (`SBAC`) parent
with one Sample (`SBNK`) child, plus one direct `SBNK` assignment. Both Samples
reference one shared mono Wave Data object (`SMPL`); the Program resolved both
channel-specific assignments, and
both paths loaded and played. This promotes that exact complete hierarchy, not
arbitrary group sizes, Program counts, or graph shapes.

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

Physical Yamaha hardware has enumerated the generated group and volume, loaded
the transferred Program, Sample Banks, and Samples and resolved their transferred Wave Data
relationships, and produced audible playback. The transferred Sequence was
also visible. This promotes the exact whole-floppy Yamaha-object transfer
profile through loading and audition, while retaining the boundary above:
non-object files and FAT filesystem metadata are not transferred.

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

Top-level HDS fields:

| Field | Rule |
| --- | --- |
| `schema_version` | Required; currently exactly `"1.0"`. |
| `size_bytes` | Required integer from 1 MiB through 2 GiB, divisible by 512. The starter uses 512 MiB. |
| `partitions` | Required array containing `1..8` partition objects. |

HDS partition and volume fields:

| Field | Rule |
| --- | --- |
| partition `name` | Required non-empty sampler-facing partition name. |
| partition `volumes` | Required array of volume objects; it may be empty. |
| volume `name` | Required non-empty sampler-facing volume name. |
| volume `waveforms` | Required array; it may be empty. |
| volume `sample_banks` | Required array; it may be empty. |
| volume `sample_bank_groups` | Optional array using the common authored-content schema below. |
| volume `programs` | Optional array using the common authored-content schema below. |

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
| `waveforms` | Required array. Completed FAT12 images must contain at least one generated object. An object-empty ISO volume is accepted only as a package-import staging target; it is not a hardware-promoted standalone profile. |
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
contains 1..3 mono Samples. Each Program has exactly two ordered
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

## Alteration Manifest

`alter hds` accepts a strict versioned JSON document with one or more ordered
operations. Generate the starter instead of guessing field names:

```bash
axklib alter manifest -o transaction.json
```

The starter contains one valid rename operation with placeholder names:

```json
{
  "schema_version": "1.0",
  "operations": [
    {
      "id": "rename-waveform",
      "type": "rename_waveform",
      "partition_index": 0,
      "volume_name": "Volume",
      "waveform_name": "Old Wave",
      "new_waveform_name": "New Wave"
    }
  ]
}
```

Every operation has a unique `id`, a `type`, and a `partition_index`. A numeric
partition index is `0..7`. An operation may instead use the partition selected
by an earlier operation:

```json
"partition_index": {"operation_ref": "earlier-operation-id"}
```

References are backward-only. The complete transaction is planned in order, so
later operations see the evolving result of earlier operations.

Supported operation types:

| Type | Required operation-specific fields |
| --- | --- |
| `delete_volume` | `volume_name` |
| `insert_volume` | `volume` using the common authored-volume schema |
| `rename_volume` | `volume_name`, `new_volume_name` |
| `delete_waveform` | `volume_name`, `waveform_name` |
| `insert_waveform` | `volume_name`, `audio` |
| `rename_waveform` | `volume_name`, `waveform_name`, `new_waveform_name` |
| `delete_sbnk` | `volume_name`, `sample_bank_name` |
| `insert_sbnk` | `volume_name`, `sample_bank` |
| `rename_sbnk` | `volume_name`, `sample_bank_name`, `new_sample_bank_name` |
| `delete_sbac` | `volume_name`, `sample_bank_group_name` |
| `insert_sbac` | `volume_name`, `sample_bank_group` |
| `rename_sbac` | `volume_name`, `sample_bank_group_name`, `new_sample_bank_group_name` |
| `delete_program` | `volume_name`, `program_number` |
| `insert_program` | `volume_name`, `program` |

An `insert_waveform` audio object contains `path`, one or two distinct
`waveform_names`, and `root_key`; `target_sample_rate` is optional. Relative
audio paths resolve from the alteration manifest directory.

An `insert_sbnk` object contains `name`, `waveform_name`, `root_key`, `key_low`,
and `key_high`. Optional fields are `right_waveform_name` and `level`. The named
Wave Data entries in the manifest's `waveforms` array must already exist at
that point in the ordered transaction.

An `insert_sbac` object contains `name` and `member_sample_banks`, an array of
one to three distinct existing Sample Bank names. An `insert_program` object
contains a Program `number` and exactly two assignments: a
`sample_bank_group` on receive channel 1 followed by a direct `sample_bank` on
receive channel 2. These limits match the currently supported authored profile.

Plan without writing an image:

```bash
axklib alter hds source.hds transaction.json --pretty
```

Apply to a different output path only after reviewing that plan:

```bash
axklib alter hds source.hds transaction.json -o altered.hds --pretty
```

Deletion checks live relationships. Delete a Program or group before deleting
objects it owns, and delete a Sample Bank before deleting its waveform. The
engine rejects an operation that would leave a known dangling relationship.

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
The server application API normally requires a distinct output file. Trusted
clients may instead set `replaceSource` to `true` on `alter.hds`, set `output`
to the same `FileRef` as `source`, and close every open session for that image
first. The operation then
builds and validates a temporary sibling before atomically replacing the source
path; `overwrite` must be omitted in this mode. `alter.inspect` provides a
write-free advisory validation response. It does not issue a token or authorize
a later apply request; `alter.hds` receives and revalidates the complete request.
