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

Run any create command with `--dry-run` first to validate the complete manifest
and its bound inputs through the canonical build planner without creating the
output:

```bash
axklib create hds image.json --output HD00_512_generated.hds --dry-run
axklib create floppy floppy.json --output generated.ima --dry-run
axklib create iso cdrom.json --output generated.iso --dry-run
```

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
- `floppy` contains one Wave Data object and Sample skeleton referring to
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
  "samples": [
    {
      "name": "Tone Sample",
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
sequence without resampling or requantization. Linear PCM8 is expanded exactly
to PCM16 without dither; current writer profiles do not emit one-byte `SMPL`
Wave Data. The source container and byte order still change when samples are
serialized as a Yamaha `SMPL` object.

WAV, FLAC, and AIFF input is accepted when it contains linear 8-, 16-, 24-, or
32-bit integer PCM, or 32-/64-bit floating-point PCM. Compressed and unknown
subtypes are rejected. Higher-precision input is reduced to PCM16 with the
versioned `axk-tpdf-pcg32-v1` dither policy. The supported output rates are
4,000, 5,512, 6,000, 8,000, 11,025, 12,000, 16,000, 22,050, 24,000, 32,000,
44,100, and 48,000 Hz. A supported source rate is preserved by default; an
unsupported source rate defaults to 44,100 Hz. Explicit conversion to any
supported rate uses pinned libsoxr VHQ processing and the same deterministic
dither policy. See
[Sampler Data Structures](sampler-data.md) for the generated object fields and
stored PCM representation.

Names written into Yamaha object and menu fields must be ASCII and at most 16
bytes. Manifest IDs such as waveform `id` are manifest-local references and do
not become sampler-facing names.

### Wave Data Size Limit

Each physical `SMPL` Wave Data channel may contain at most 16,777,216 logical
PCM16 frames, or 32 MiB after conversion. A mono Sample may reference one such
channel. A stereo Sample may reference two channels, for at most 64 MiB total,
but its maximum duration is unchanged because each channel is checked
separately.

The limit applies to the converted sampler data, not the source file size.
Resampling can therefore move a source across the boundary. Audio inspection
reports source/output widths, the selected output rate, dither policy,
`projectedOutputFrameCount`, `projectedOutputBytesPerChannel`,
`projectedOutputBytesTotal`, `maximumOutputFrameCountPerChannel`,
`maximumOutputBytesPerChannel`, `valid`, and structured `issues` before an
import is applied. Creation and alteration reject oversized audio before
decoding its full payload, and the low-level writer rechecks the limit before
serializing an `SMPL` object.

Source decoding is separately bounded to 16,777,216 frames per channel and
256 MiB of decoded intermediate samples. This bound is checked from container
metadata before allocating the decode buffer, including when downsampling would
produce a much smaller Yamaha Wave Data object.

### WAV Sampler Metadata

Audio inspection reports `samplerDefaults` in addition to storage projections.
For WAV input, axklib maps a usable `smpl` unity note, pitch fraction, and
single forward loop to the A-series root key, fine tune, and forward-loop
window. Loop endpoints in `smpl` are inclusive; axklib converts them to the
A-series start/length representation and rescales the boundaries when the
audio is resampled. A nonzero `smpl` repeat count is normalized to the A-series
indefinite forward-loop mode and reported as the non-fatal
`wav_sampler_loop_repeat_count_normalized` adjustment. The range is retained
because A-series Samples cannot represent a finite repeat count but can safely
represent the forward loop itself.

A WAV `inst` chunk supplies root key and fine tune only when `smpl` does not,
and supplies the Sample key and velocity ranges. `smpl` wins a pitch conflict.
Multiple, backward, alternating, malformed, out-of-range, or resampling-empty
loops are not approximated: inspection reports a non-fatal issue and defaults
to the hardware-proven forward one-shot mode.

Files without usable sampler metadata also default to forward one-shot. The
inspection result records `pitchSource`, `rangeSource`, and `loopSource`, so a
client can distinguish WAV-authored values from A-series defaults before
writing. Explicit manifest values remain authoritative after inspection.

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
    "samples": [
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
hardware through group and volume enumeration, Sample loading, audible
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
    "samples": [
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
FAT copies, a 224-entry root directory, one DOS 8.3 root file per Yamaha object,
one synthesized `YAMAHA.SYM`, and the zero-length standalone-disk marker
`A3000_SY.001`. The `authored_volume.name` value supplies the Yamaha disk label;
axklib displays the object scope as `FAT root`.

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

Transfer planning inventories object metadata and relationships before it
loads payloads. For `selection: "roots"`, only the selected dependency closure
is loaded. The C++ engine's `MediaBuildLimits` and the shared SDK's
`media_build_limits` bound each object, all prepared payloads together, and the
completed output. Their defaults are 64 MiB per object and 737,280,000 bytes
for both aggregate payloads and output. Supplying a limits object to
`plan_media_build()` or `build_plan::from_manifest()` makes object and aggregate
payload admission part of planning; the output limit is checked against the ISO
projection before the temporary file is resized. The same limits remain
attached to an SDK plan during apply. Limits may be lowered for a constrained
host but must remain nonzero, and the object limit cannot exceed the aggregate
payload limit.

Physical Yamaha hardware has enumerated the generated group and volume, loaded
the transferred Program, Sample Banks, and Samples, resolved their transferred
Wave Data relationships, and produced audible playback. A byte-preserved
transferred Sequence has also loaded and played successfully on an A4000. This
promotes exact whole-floppy Yamaha-object transfer for the admitted Program,
Sample Bank, Sample, Wave Data, and Sequence profiles. MIDI-authored Sequence
creation and Sequence rename/save-back remain bounded by their current hardware
writer profile. Non-object files and FAT filesystem metadata are not
transferred.

## Convert HDA/HDS Content In axkdeck

Axkdeck exposes container conversion on file-backed SFS images through the
object tree context menu:

- Right-click a partition and choose **Export CD-ROM image...** to convert the
  complete partition to an ISO9660 image.
- Right-click a partition and choose **Export volumes to floppies...** to
  convert every immediate volume in one batch.
- Right-click a volume and choose **Export floppy image...** to convert the
  complete volume to a 1,474,560-byte FAT12 image.

Both workflows first show a bounded inspection with selected object counts,
payload size, output capacity, and every blocking issue. A volume that fits one
floppy produces a raw `.ima`; an admitted larger volume produces an ordered
`axklib.floppy-disk-set.v1` ZIP. Multi-floppy ordering is dependency-aware:
Programs precede Sample/first-use-Wave pairs, remaining Wave Data, Sample Banks,
and Sequences. A whole first-use Wave Data object that moves to the next member
does so without repeating its Sample and carries the destination member's
two-digit suffix in its logical catalog path. Unrelated whole-object rollover
does not gain that suffix. `A3000F.SYM` marks only a Wave Data file that
continues across that boundary; ordinary whole-object rollover uses
`A3000.SYM`, and the final member uses `A3000E.SYM`. A partition is never
reduced to a subset without an explicit future selection contract. The destination chooser
matches package export:
**Storage location** publishes to a configured workspace and **This computer**
streams the retained result to the desktop file chooser. Suggested names use
the zero-based partition index and the selected partition or volume name.

Conversion rebuilds only the destination container. Yamaha Program, Sample
Bank, Sample, Wave Data, and Sequence payloads are copied byte for byte from the
selected source scope. The ISO path uses reader-backed streaming and does not
materialize the selected payload set in memory. FAT12 members use fixed floppy
capacity and multi-floppy output is capped at 32 images.

Partition batch export does not define a second floppy format. It plans the
partition once, then applies the individual volume conversion contract to each
immediate volume. Each successful volume receives a collision-safe directory
containing raw `diskNN.ima` files. Thus a single-floppy volume has
`disk01.ima`, while a multi-floppy volume has `disk01.ima` through its final
member without an inner ZIP. The root `volume-floppies.axklib.json` report
records every exported, empty, blocked, or failed volume. Failures are isolated
per volume and zero-success runs publish no directory or download.

The public native entry points are `plan_volume_floppy_export` and
`write_volume_floppy_export`. They share one source parse, inventory, catalog,
and relationship graph across the operation. Multi-floppy members are built by
the same member builder used by `write_media_conversion`, so corresponding raw
images are byte-identical rather than merely semantically equivalent.

Generated ISO partition conversion has hardware-promoted one-volume and
multi-volume profiles. A four-volume conversion was enumerated, loaded, and
auditioned completely on physical A4000 hardware. Directory extents and both
path tables are planned and emitted across as many complete 2048-byte sectors
as their deterministic records require. Multi-sector output has host reopen,
external-tool, and exact-payload coverage; its object-heavy hardware profile is
documented separately until that physical verification is complete.

Dependency closure remains Known-only. One bounded whole-partition exception
does not create a dependency: an active-form Program assignment row whose exact
named Sample or Sample Bank target is absent from the complete source partition
is copied byte for byte and reported as a nonblocking retained-disabled-row
warning. This preserves sampler-saved Program bytes without redirecting the row
to a similarly named object. An exact candidate with non-Known quality,
source-load row, out-of-scope target, or cross-volume target remains blocking.
Inspection admits conversion only when no blocking issue remains; nonblocking
retention warnings stay visible in the destination dialog.

Multi-volume output remains subject to exact placement, the relationship rules
above, object identifier counts, payload limits, and the 700 MB capacity check
reported by inspection.
Generated floppy output is host-reopened and payload-compared, but fresh FAT12
authoring still retains the hardware-validation qualification documented above.
Multi-floppy output also validates dependency-derived order, absence of
unintended complete-object duplication, and byte-exact reassembly of all split
Wave Data.

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
    "root_object_keys": ["<sample-object-key>"]
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
| `schema_version` | Required; the only accepted value is `"1.0"`. |
| `size_bytes` | Required integer from 1 MiB through 2 GiB, divisible by 512. The starter uses 512 MiB. |
| `partitions` | Required array containing `1..8` partition objects. |

HDS partition and volume fields:

| Field | Rule |
| --- | --- |
| partition `name` | Required non-empty sampler-facing partition name. |
| partition `volumes` | Required array of volume objects; it may be empty. |
| volume `name` | Required non-empty sampler-facing volume name. |
| volume `waveforms` | Required array; it may be empty. |
| volume `samples` | Required Sample (`SBNK`) array; it may be empty. |
| volume `sample_banks` | Optional Sample Bank (`SBAC`) array using the common authored-content schema below. |
| volume `programs` | Optional array using the common authored-content schema below. |

Top-level removable-media fields:

| Field | Rule |
| --- | --- |
| `schema_version` | Required; the only accepted value is `"1.0"`. |
| `format` | Required; `"fat12_floppy"` or `"iso9660"`. |
| `authored_volume` / `transfer` | Exactly one is required. |
| `iso` | Required for `iso9660`; omitted for `fat12_floppy`. |

`authored_volume` fields:

| Field | Rule |
| --- | --- |
| `name` | Required non-empty string. Match `iso.volume_name` for clear ISO manifests. |
| `waveforms` | Required array. Completed FAT12 images must contain at least one generated object. An object-empty ISO volume is accepted only as a package-import staging target; it is not a hardware-promoted standalone profile. |
| `samples` | Required Sample (`SBNK`) array. |
| `sample_banks` | Optional Sample Bank (`SBAC`) array; each bank contains 1..127 distinct Sample names. |
| `programs` | Optional array; Program numbers are `1..128`. |

Waveform fields:

| Field | Rule |
| --- | --- |
| `id` | Required unique manifest-local string. |
| `name` | Required ASCII sampler object name, at most 16 bytes. |
| `path` | Required WAV, FLAC, or AIFF source path. Relative paths use the manifest directory. |
| `root_key` | Required MIDI note `0..127`. |
| `target_sample_rate` | Optional requested output rate. Omit to preserve supported native rates or use the default conversion policy. |
| `fine_tune_cents` | Optional signed fine tune `-63..63`; default `0`. |
| `loop_mode` | Optional A-series mode: `1` forward loop or `4` forward one-shot; default `4`. |
| `loop_start_frame`, `loop_length_frames` | Optional explicit loop window. Forward loop requires a non-empty contained range. One-shot requires both manifest values to remain zero and serializes the full physical Wave Data span. |

Direct and stereo Sample fields:

| Field | Rule |
| --- | --- |
| `name` | Required unique ASCII `SBNK` name, at most 16 bytes. |
| `root_key` | Required MIDI note `0..127`. |
| `key_low`, `key_high` | Required MIDI limits `0..127`; high must not precede low. |
| `level` | Optional `0..127`; default `100`. |
| `fine_tune_cents` | Optional signed fine tune `-63..63`; default `0`. |
| `velocity_low`, `velocity_high` | Optional MIDI limits `0..127`; defaults `0` and `127`, and high must not precede low. |
| `loop_mode` | Optional A-series mode: `1` forward loop or `4` forward one-shot; default `4`. |
| `loop_start_frame`, `loop_length_frames` | Optional Sample loop window within the full linked Wave Data playback span. Forward loop requires a non-empty contained range. One-shot requires both manifest values to remain zero. |
| `waveform_id` | Direct left/mono member. Mutually exclusive with `interleaved_audio_path`. |
| `right_waveform_id` | Optional direct right member; it must differ from `waveform_id`. |
| `interleaved_audio_path` | Alternative two-channel source that generates linked left/right `SMPL` objects. |
| `left_waveform_name`, `right_waveform_name` | Optional names for generated interleaved members. |
| `target_sample_rate` | Optional conversion target for interleaved input. |

Direct stereo members must have equal sample rate and logical frame count.
Interleaved input is split into two physical mono objects and inherently meets
that constraint.

The current authored `SBAC`/`PROG` profile is intentionally narrow. Each Sample Bank
contains 1..127 mono or stereo Samples. Each Program has exactly two ordered
assignments: one distinct `sample_bank` on receive channel `1`, followed
by one direct `sample` on receive channel `2`. Every Sample Bank and direct Sample
used by the Program profile is assigned once, and the direct Program Sample remains
mono-only. Sequence (`SEQU`) and profile (`PRF3`)
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
| `rename_partition` | `partition_name`, `new_partition_name` |
| `delete_waveform` | `volume_name`, `waveform_name` |
| `insert_waveform` | `volume_name`, `audio` |
| `rename_waveform` | `volume_name`, `waveform_name`, `new_waveform_name` |
| `delete_sbnk` | `volume_name`, `sample_name` |
| `insert_sbnk` | `volume_name`, `sample` |
| `rename_sbnk` | `volume_name`, `sample_name`, `new_sample_name` |
| `delete_sbac` | `volume_name`, `sample_bank_name` |
| `insert_sbac` | `volume_name`, `sample_bank` |
| `rename_sbac` | `volume_name`, `sample_bank_name`, `new_sample_bank_name` |
| `delete_program` | `volume_name`, `program_number` |
| `insert_program` | `volume_name`, `program` |
| `rename_program` | `volume_name`, `program_number`, `new_program_name` |

An `insert_waveform` audio object contains `path`, one or two distinct
`waveform_names`, and `root_key`. Optional fields are `target_sample_rate`,
`fine_tune_cents`, `loop_mode`, `loop_start_frame`, and
`loop_length_frames`. Relative audio paths resolve from the alteration manifest
directory.

An `insert_sbnk` object contains `name`, `waveform_name`, `root_key`, `key_low`,
and `key_high`. Optional fields are `right_waveform_name`, `level`,
`fine_tune_cents`, `velocity_low`, `velocity_high`, `loop_mode`,
`loop_start_frame`, and `loop_length_frames`. The named Wave Data entries in
the evolving transaction must already exist at that point.

An `insert_sbac` object contains `name` and `member_samples`, an array of
one to 127 distinct existing Sample names. Samples may be mono or stereo. If a
member already belongs to another Sample Bank, the transaction removes that
membership and moves the Sample into the new bank; the source bank remains with
its other members and may become empty. A Sample assigned directly to a Program
is rejected rather than silently changing the Program assignment. An
`insert_program` object contains a Program `number` and exactly two assignments: a
`sample_bank` on receive channel 1 followed by a direct `sample` on
receive channel 2. These limits match the currently supported authored profile.
`rename_program` changes the sampler-visible Program name while retaining its
numeric slot; `new_program_name` is `1..8` printable ASCII characters without
leading or trailing spaces.

Plan without writing an image:

```bash
axklib alter hds source.hds transaction.json --pretty
```

Apply to a different output path only after reviewing that plan:

```bash
axklib alter hds source.hds transaction.json -o altered.hds --pretty
```

Deletion checks live relationships. Delete a Program or Sample Bank before deleting
objects it owns, and delete a Sample before deleting its Wave Data. The
engine rejects an operation that would leave a known dangling relationship.

## Canonical Terminology

Schema `1.0` is the canonical authored and alteration format. It uses `samples`
for Sample (`SBNK`) objects, `sample_banks` for Sample Bank (`SBAC`) objects,
`member_samples` for bank membership, and Program targets `sample` and
`sample_bank`. Obsolete pre-release field meanings are rejected rather than
translated.

The C++ writer API follows the same terminology: `SampleSpec` models `SBNK`,
`SampleBankSpec` models `SBAC`, and `VolumeSpec` exposes `samples` and
`sample_banks`. No transitional C++ aliases are provided for superseded
pre-release names.

## Publication And Validation Guarantees

Both removable-media writers:

1. build into a temporary sibling of the requested output;
2. reopen the temporary image through the production reader;
3. compare the complete expected and reopened object-payload multisets;
4. publish only after those checks pass.

ISO sectors are written directly to the reserved temporary file in bounded
chunks. Reopen validation inventories metadata first and hashes retained files
and object payloads one at a time. The writer therefore does not allocate a
second output-sized image buffer; prepared payload memory and output size are
still independently bounded by `MediaBuildLimits`.

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
