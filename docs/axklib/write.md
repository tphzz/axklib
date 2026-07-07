# Image Writer

`axklib.write` contains the generated-image API for creating fresh Yamaha SFS/HDS
images. The first supported scope is deliberately small: partitions, volumes,
mono current `SMPL` waveforms, and direct single-member `SBNK` sample banks.

The writer serializes the image from its data model. It does not patch existing
images and does not depend on template images.

Generated `SMPL` waveform payloads and `SBNK` sample-bank payloads use the
current object-header sizes, link fields, and single-member default parameter
block required for ordinary A-series sample storage. The `SMPL` stored payload
contains the logical mono WAV frames plus a short compatibility tail; the
sample-window fields continue to describe the logical frame count.

The writer exposes tested profiles instead of guessing general disk
geometry from one image. Partition slots are capped at 1 GiB. The currently
supported hard-disk profiles are:

- 256 MiB, one SFS partition;
- 512 MiB, two SFS partitions using the tested two-partition metadata profile;
- 768 MiB, three SFS partitions using the sparse formatter profile for empty volume skeletons.

These profiles initialize sector 2 or the corresponding sparse marker sectors,
partition metadata, primary/duplicate partition headers, allocation bitmaps,
directory indexes, and volume directories. Object-record writing is currently
validated on the 256 MiB and 512 MiB profiles; the 768 MiB sparse profile is
limited to empty volume skeletons until object payloads are validated on that
profile. The 256 MiB profile has been hardware-tested with multiple volumes,
current waveforms, and direct single-member sample banks. The 512 MiB profile
has been hardware-tested with two generated volumes per partition and two
generated direct sample banks per volume, plus isolated per-volume growth to
eight generated direct sample banks in one volume on either partition.
Root key, key range, and sample level have been hardware-tested for generated
direct single-member sample banks. The writer is intentionally conservative:
unsupported object types, multi-member sample-bank groups, untested hard-disk
metadata profiles, and in-place image mutation are outside the current API.

After creating an image, validate it with the public reader before trying it on
hardware:

```powershell
uv run axklib info .\HD00_512_writer_test.hds
uv run axklib validate .\HD00_512_writer_test.hds -o .\writer-validate
uv run axklib extract wav file .\HD00_512_writer_test.hds -o .\writer-wavs
```

::: axklib.write
