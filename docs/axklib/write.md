# Image Writer

`axklib.write` contains the generated-image API for creating fresh Yamaha SFS/HDS
images. The supported object scope is deliberately small: partitions, volumes,
mono current `SMPL` waveforms, direct single-member `SBNK` sample banks, and
equal-format two-member stereo `SBNK` sample banks. A narrow hardware-tested
profile also writes one-to-three-member `SBAC` groups and Program assignments.

The writer serializes the image from its data model. It does not patch existing
images and does not depend on template images.

Generated `SMPL` waveform payloads and `SBNK` sample-bank payloads use the
current object-header sizes, link fields, and default parameter block required
for ordinary A-series sample storage. `SBNK` payloads are
serialized through the public sample-bank contract model: active member fields,
key range, and level come from the writer data model, while fields not yet
exposed by the write API keep conservative current-format defaults for the
single-member parameter block, sample-control block, and reserved Sample
Parameter defaults required for current direct-bank playback and tone. The `SMPL`
stored payload contains the logical mono audio frames plus a short compatibility
tail; the sample-window fields continue to describe the logical frame count.
The writer supports 512-byte-aligned image sizes from 1 MiB through 2 GiB and
one through eight equal SFS partition slots. For `N` partitions it computes:

```text
total_sectors = size_bytes / 512
slot_span = min(floor((total_sectors - 2) / N), 0x1fffff)
partition_start[i] = 3 + i * slot_span
partition_sector_count = slot_span - 1
```

The two sectors before the first slot hold the disk superblock and sector-2
formatter-transfer metadata. Each later slot begins with an equivalent transfer
sector immediately before its partition header. Division remainder and space
beyond the 1 GiB slot cap remain as unused image-tail sectors. Every slot must
contain at least 2045 partition sectors, so the exact minimum image size for
`N` partitions is `(2 + N * 2046) * 512` bytes. Content must also fit after the
partition's bitmap, directory index, and volume metadata.

The writer initializes sector 2, later pre-partition transfer sectors,
partition metadata, primary/duplicate partition headers, allocation bitmaps,
directory indexes, and volume directories. Leading transfer tokens are
deterministic compatibility bytes (`ab432100` through `ab432107`); they are not
disk geometry or persistent disk IDs. Prior-token residue at `+0x09..+0x10`
and unneeded sector-2 label-entry record areas remain zeroed. The equal-slot
algorithm has been validated across image-size, partition-count, remainder,
1 GiB slot-cap, and minimum-size boundaries.

Generated images have also been hardware-tested with multiple volumes, current
waveforms, direct single-member sample banks, template-free two-member stereo
sample banks, and isolated object-count growth.
Root key, key range, and sample level have been hardware-tested for generated
direct single-member sample banks. The writer is intentionally conservative:
unsupported object types, sample banks with more than two members, and in-place
image mutation are outside the current API. SBAC/PROG output is restricted to
the exact per-Program profile below; broader assignment orders, receive
channels, grouped stereo banks, and groups with more than three members are rejected.

## Audio importing

`VolumeBuilder.add_waveform_from_audio()` accepts a mono source in any container
and subtype supported by the installed libsndfile build. The existing
`add_waveform_from_wav()` method uses the same importer. Sources are converted
to signed 16-bit PCM without normalization. Native PCM16 at a supported rate is
preserved byte-for-byte; higher precision, floating-point, and resampled audio
is quantized with deterministic TPDF dither.

The importer preserves `4000`, `5512`, `6000`, `8000`, `11025`, `12000`,
`16000`, `22050`, `24000`, `32000`, `44100`, and `48000` Hz. Other source rates
are converted to `44100` Hz with the soxr VHQ profile. Pass
`target_sample_rate=` to select a different rate from the same set. Sources
with more than two channels are rejected rather than implicitly downmixed.

Use `add_stereo_sample_bank_from_audio()` to create a two-member stereo SBNK
directly from one interleaved source. Channel 1 becomes the left physical SMPL
and channel 2 becomes the right physical SMPL. No intermediate mono files are
written. By default, waveform names use the first 14 ASCII bytes of the bank
name plus `-L` and `-R`; explicit names can override them.

```python
bank = volume.add_stereo_sample_bank_from_audio(
    name="Stereo Piano",
    path="stereo-piano.flac",
    root_key=60,
    key_low=21,
    key_high=108,
    level=100,
)
```

`HdsWriteResult.audio_imports` records the source format, subtype, channels,
source and output rates, output frames, conversion actions, generated waveform
names, and clipped-sample count. Finite overshoots are clipped and reported;
NaN and infinite samples are rejected.

## SBAC and PROG profile

The first template-free SBAC/PROG profile is intentionally exact:

- any supported 512-byte-aligned image size and one through eight partitions;
- any number of independently configured volumes per partition;
- one or more `SBAC` groups with one through three mono `SBNK` children;
- one Program in range `001..128` per group, with unique Program numbers;
- each Program's assignment 1 targets its group on receive channel `1`;
- each Program's assignment 2 targets a unique direct mono `SBNK` on receive
  channel `2`;
- for one-member groups, the grouped child and direct control use the same
  waveform and musical parameters; multi-member groups may use an independent
  direct control, and every group is assigned exactly once.

The writer emits zero for runtime handle caches. Every grouped child receives
the sample-bank-member flag, while only the direct assignment updates the SBNK
Program bitmap.

```python
from axklib.write import HdsImageBuilder

builder = HdsImageBuilder(size_bytes=1024 * 1024)
volume = builder.add_partition("New Partition").add_volume("New Volume")
waveform = volume.add_waveform_from_wav(
    name="pulse 1", path="tone.wav", root_key=66
)
member = volume.add_sample_bank(
    name="JS01", waveform=waveform, root_key=66,
    key_low=0, key_high=127, level=100,
)
control = volume.add_sample_bank(
    name="JS02 *", waveform=waveform, root_key=66,
    key_low=0, key_high=127, level=100,
)
group = volume.add_sample_bank_group(name="AUDSB", member=member)
program = volume.add_program(number=1)
program.assign_sample_bank_group(group, receive_channel=1)
program.assign_sample_bank(control, receive_channel=2)
builder.write("HD00_512_generated.hds")
```

Partition headers are zero-initialized and populated through explicit field
writes. Former fixed nonzero tail bytes are deliberately left zero after
object-bearing 256 MiB / one-partition and 512 MiB / two-partition hardware
checks showed that volumes, sample banks, parameters, and playback remain
correct without them.

## JSON build manifests

`axklib create hds` builds the same typed model from a versioned JSON manifest.
Waveform paths are resolved relative to the manifest file. Waveform IDs are
local references within one volume; they are not written as sampler object
names or filesystem IDs.

A stereo bank may reference two distinct mono waveform entries. `waveform_id`
is the left member and `right_waveform_id` is the right member. The importer
converts each source to sampler-compatible PCM, after which both members must
have matching sample rate and logical frame count. The image stores two
physical mono `SMPL` objects linked by one two-member `SBNK`; exact extraction
retains both and can additionally render an interleaved stereo WAV.

```json
{
  "schema_version": "1.0",
  "size_bytes": 268435456,
  "partitions": [
    {
      "name": "hd1",
      "volumes": [
        {
          "name": "New Volume",
          "waveforms": [
            {
              "id": "tone",
              "name": "Tone",
              "path": "audio/tone.wav",
              "root_key": 60
            }
          ],
          "sample_banks": [
            {
              "name": "Tone Bank",
              "waveform_id": "tone",
              "root_key": 60,
              "key_low": 60,
              "key_high": 60,
              "level": 100
            }
          ]
        }
      ]
    }
  ]
}
```

For a stereo bank, declare both member waveforms and add
`"right_waveform_id": "right"` to the bank. The typed API provides the same
contract through `VolumeBuilder.add_stereo_sample_bank()`.

Alternatively, create both physical members from one interleaved source. This
form is mutually exclusive with `waveform_id` and `right_waveform_id`:

```json
{
  "name": "Stereo Piano",
  "interleaved_audio_path": "audio/stereo-piano.flac",
  "left_waveform_name": "Piano-L",
  "right_waveform_name": "Piano-R",
  "target_sample_rate": 44100,
  "root_key": 60,
  "key_low": 21,
  "key_high": 108,
  "level": 100
}
```

Mono waveform entries also accept optional `target_sample_rate`. Audio paths
are resolved relative to the manifest file. The create command reports every
audio import and whether it was split, resampled, quantized, or passed through.

For the SBAC/PROG profile, add these optional volume fields after declaring the
mono sample banks. The singular field remains available for one-member groups;
use `member_sample_banks` for two or three children:

```json
{
  "sample_bank_groups": [
    {"name": "AUDSB", "member_sample_bank": "JS01"}
  ],
  "programs": [
    {
      "number": 1,
      "assignments": [
        {"sample_bank_group": "AUDSB", "receive_channel": 1},
        {"sample_bank": "JS02 *", "receive_channel": 2}
      ]
    }
  ]
}
```

For a group whose declared sample banks are `Member C3`, `Member D3`, and
`Member E3`, use:

```json
{"name": "Key Group", "member_sample_banks": ["Member C3", "Member D3", "Member E3"]}
```

Build the image with:

```powershell
uv run axklib create hds .\image.json -o .\HD00_512_generated.hds
```

The command refuses to replace an existing image unless `--overwrite` is
provided. It reports each partition's start, sector count, cluster count,
sampler-visible free KiB, and any unused image-tail sectors. Each
`WrittenPartitionLayout` also exposes `first_payload_cluster`,
`allocated_cluster_count`, `free_cluster_count`, and `free_bytes`. Manifest
parsing is strict: unknown fields, duplicate
waveform IDs, unresolved waveform references, invalid MIDI values, and
invalid geometry and content that cannot fit its partition are rejected before
a usable image is reported. Group/member and Program-assignment references are
also validated before the hardware-profile constraints are applied.

After creating an image, validate it with the public reader before trying it on
hardware:

```powershell
uv run axklib info .\HD00_512_writer_test.hds
uv run axklib validate .\HD00_512_writer_test.hds -o .\writer-validate
uv run axklib extract wav file .\HD00_512_writer_test.hds -o .\writer-wavs
```

## Altering existing HDS images

`axklib alter hds` executes a versioned, ordered transaction against an
existing HDS image. The current operation set supports deleting and inserting
complete volumes and deleting or inserting individual sample banks. Inserted
volumes use the same volume schema and audio conversion behavior as fresh-image
build manifests. Sample-bank insertion references physical waveform objects
that already exist in the selected volume; it does not duplicate their PCM.

Omit `-o` to parse, resolve, allocate, and validate the transaction without
writing an image:

```powershell
uv run axklib alter hds .\source.hds .\transaction.json
```

Supply a new output path to apply the transaction. The source is never changed.
The command writes a temporary sibling file, validates the resulting filesystem,
and publishes the output atomically. It refuses an existing destination and
does not provide an in-place or overwrite mode.

```powershell
uv run axklib alter hds .\source.hds .\transaction.json -o .\altered.hds
```

A transaction may contain one or more operations:

```json
{
  "schema_version": "1.0",
  "operations": [
    {
      "id": "remove-old",
      "type": "delete_volume",
      "partition_index": 0,
      "volume_name": "Old Volume"
    },
    {
      "id": "insert-new",
      "type": "insert_volume",
      "partition_index": {"operation_ref": "remove-old"},
      "volume": {
        "name": "New Volume",
        "waveforms": [
          {
            "id": "tone",
            "name": "Tone",
            "path": "audio/tone.wav",
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
    }
  ]
}
```

Operation IDs must be unique. An `operation_ref` must point backward and uses
the earlier operation's partition scope; forward references are rejected.
Operations observe the logical results of every preceding operation, so a
transaction can delete a volume and insert a replacement with the same name.

Volume deletion requires one exact partition/name match and a completely
readable directory closure. A missing record, unresolved extent chain, or
reference into that closure from another directory rejects the whole
transaction. Unrelated records, including unreferenced records that axklib does
not decode, are preserved. Deleted index and allocation ownership is cleared,
while freed payload bytes are left untouched.

Insertion uses the lowest available SFS record IDs and lowest-cluster first-fit
allocation. Fragmented payloads use direct extents where possible and chained
continuation-list clusters when necessary. The existing root directory must be
readable through direct extents, and the updated root must fit its existing
allocation; this version does not relocate the root directory.

### Sample-bank operations

Delete one sample bank by exact partition, volume, and sampler-visible bank
name:

```json
{
  "id": "delete-unused-bank",
  "type": "delete_sbnk",
  "partition_index": 0,
  "volume_name": "Drums",
  "sample_bank_name": "Unused Snare"
}
```

Deletion is rejected when the bank has a Program-link bitmap, appears in an
`SBAC` member slot, appears in a Program SBNK assignment, or has another Known
incoming Program/SBAC relationship. Only the SBNK directory entry, index
record, and allocation ownership are removed. Its linked physical waveforms
remain present and may consequently become orphans for later analysis.

Insert a mono bank that references an existing waveform by its exact name in
the volume's `SMPL` category:

```json
{
  "id": "insert-bank",
  "type": "insert_sbnk",
  "partition_index": 0,
  "volume_name": "Drums",
  "sample_bank": {
    "name": "New Snare",
    "waveform_name": "Snare Wave",
    "root_key": 60,
    "key_low": 36,
    "key_high": 84,
    "level": 100
  }
}
```

For a two-member stereo bank, add `right_waveform_name`. Both physical members
must exist uniquely in the same volume and have matching sample rate and frame
count. The new SBNK takes its link IDs, sample rate, frame count, fine tune, and
loop window from those current SMPL records. Bank root key, key range, and level
come from the transaction. Duplicate bank names and unresolved waveform names
are rejected before output is written.

::: axklib.write

::: axklib.build_manifest

::: axklib.alteration
