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
stored payload contains the logical mono WAV frames plus a short compatibility
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

A stereo bank references two distinct mono waveform entries. `waveform_id` is
the left member and `right_waveform_id` is the right member. Both inputs must be
16-bit mono WAVs with matching sample rate and logical frame count. The image
stores them as two physical mono `SMPL` objects linked by one two-member `SBNK`;
it does not store an interleaved WAV. Exact extraction retains both physical
WAVs and can additionally render an interleaved stereo WAV.

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

::: axklib.write

::: axklib.build_manifest
