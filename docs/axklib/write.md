# Image Writer

`axklib.write` contains the generated-image API for creating fresh Yamaha SFS/HDS
images. The first supported scope is deliberately small: partitions, volumes,
mono current `SMPL` waveforms, and direct single-member `SBNK` sample banks.

The writer serializes the image from its data model. It does not patch existing
images and does not depend on template images.

Generated `SMPL` waveform payloads and `SBNK` sample-bank payloads use the
current object-header sizes, link fields, and single-member default parameter
block required for ordinary A-series sample storage. `SBNK` payloads are
serialized through the public sample-bank contract model: active member fields,
key range, and level come from the writer data model, while fields not yet
exposed by the write API keep conservative current-format defaults for the
single-member parameter block, sample-control block, and reserved Sample
Parameter defaults required for current direct-bank playback and tone. The `SMPL`
stored payload contains the logical mono WAV frames plus a short compatibility
tail; the sample-window fields continue to describe the logical frame count.
The writer exposes tested profiles instead of guessing general disk
geometry from one image. Partition slots are capped at 1 GiB. The currently
supported hard-disk profiles are:

- 256 MiB, one SFS partition;
- 512 MiB, two SFS partitions using the tested two-partition metadata profile;
- 768 MiB, three SFS partitions using the sparse formatter profile for empty volumes and the current narrow object-bearing profile.

These profiles initialize sector 2 or the corresponding sparse marker sectors,
while leaving unneeded sector-2 label-entry record areas zeroed. They also
initialize partition metadata, primary/duplicate partition headers, allocation bitmaps,
directory indexes, and volume directories. Object-record writing is currently
validated on the 256 MiB and 512 MiB profiles, and a narrow sparse 768 MiB /
three-partition profile has been validated for one simple object-bearing volume
on any one partition while the other partitions contain empty loadable volumes.
Profile support is not just a size and partition-count match: the
writer also emits the complete metadata profile for that supported layout. Do
not create unlisted hard-disk layouts by writing only the named superblock and
partition-header fields. The 256 MiB profile has been hardware-tested with multiple volumes,
current waveforms, and direct single-member sample banks. The 512 MiB profile
has been hardware-tested with two generated volumes per partition and two
generated direct sample banks per volume, plus isolated per-volume growth to
eight generated direct sample banks in one volume on either partition.
Root key, key range, and sample level have been hardware-tested for generated
direct single-member sample banks. The writer is intentionally conservative:
unsupported object types, multi-member sample-bank groups, untested hard-disk
metadata profiles, and in-place image mutation are outside the current API.

Partition headers are zero-initialized and populated through explicit field
writes. Former fixed nonzero tail bytes are deliberately left zero after
object-bearing 256 MiB / one-partition and 512 MiB / two-partition hardware
checks showed that volumes, sample banks, parameters, and playback remain
correct without them.

After creating an image, validate it with the public reader before trying it on
hardware:

```powershell
uv run axklib info .\HD00_512_writer_test.hds
uv run axklib validate .\HD00_512_writer_test.hds -o .\writer-validate
uv run axklib extract wav file .\HD00_512_writer_test.hds -o .\writer-wavs
```

::: axklib.write
