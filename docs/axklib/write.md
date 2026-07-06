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

For the supported 256 MiB hard-disk image profile, the writer
also initializes sector 2 and the full primary/duplicate partition-header
metadata sectors needed by the hardware load path. This generated profile has
been hardware-tested for a minimal single-partition image containing multiple
volumes, current waveforms, and direct single-member sample banks. Root key, key
range, and sample level have been hardware-tested for generated direct
single-member sample banks. The writer is intentionally conservative:
unsupported object types, multi-member sample-bank groups, other hard-disk
metadata profiles, and in-place image mutation are outside the current API.

After creating an image, validate it with the public reader before trying it on
hardware:

```powershell
uv run axklib info .\HD00_512_writer_test.hds
uv run axklib validate .\HD00_512_writer_test.hds -o .\writer-validate
uv run axklib extract wav file .\HD00_512_writer_test.hds -o .\writer-wavs
```

::: axklib.write
