# C++ And CLI Usage

## CMake Consumer

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS axklib)
add_executable(example main.cpp)
target_compile_features(example PRIVATE cxx_std_17)
target_link_libraries(example PRIVATE axklib::axklib)
```

The shared library contains the supported facade and embeds its codec,
resampling, and engine dependencies. Consumers do not find those packages.

```cpp
#include <iostream>

#include <axklib/sdk.hpp>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  axk::operation_context context;
  auto media = axk::image::open(argv[1], context);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 1;
  }
  auto objects = media->objects(0, 256, context);
  if (!objects) return 1;
  std::cout << objects->total_count << " object(s)\n";
}
```

## Inspect An Image

Begin with `info`; it uses the same names and hierarchy presented by the
sampler where those labels are available:

```bash
axklib info source.hds
axklib info source.ima
axklib info source.iso
```

Validate before altering or importing into an image:

```bash
axklib validate source.hds -o reports/source --policy strict
```

## Extract Audio And SFZ

The word `file` is the extraction scope and must precede the source path:

```bash
axklib extract wav file source.iso -o exports/wav
axklib extract sfz file source.iso -o exports/sfz
```

For one volume, Program, group, or Sample Bank, obtain stable selector paths and
pass the matching path back to extraction:

```bash
axklib info source.iso --format paths
axklib extract sfz program source.iso --path '<program-selector>' -o exports/program
```

See [Names, Paths, And Exports](names-and-paths.md) for output organization and
[SFZ Export](sfz.md) for rendering behavior.

## Move A Complete Volume Between Images

Export a complete dependency closure:

```bash
axklib package export source.hds \
  --partition 0 --volume 'Source Volume' \
  --root volume -o source-volume
axklib package verify source-volume.axkvol --format json
```

Create an empty HDS target and import the package as a new volume:

```bash
axklib create manifest hds -o target.json
axklib create hds target.json -o target.hds

axklib package plan-import target.hds source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported","create":true}' \
  --format json

axklib package import target.hds source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported","create":true}' \
  -o imported.hds --format json
```

Inspect and validate `imported.hds` before use. Package imports are volume-local
on SFS: identical physical waveform content in another volume is copied because
cross-volume waveform sharing is not a supported hardware topology.

## Author A New HDS Image

Generate the starter:

```bash
axklib create manifest hds -o image.json
```

The starter creates a 512 MiB image with one partition and one empty volume.
Add authored `waveforms`, `sample_banks`, optional `sample_bank_groups`, and
optional `programs` using the common schema in [Writer And
Alteration](write.md#common-authored-content), then build and check it:

```bash
axklib create hds image.json -o HD00_512_generated.hds
axklib info HD00_512_generated.hds
axklib validate HD00_512_generated.hds -o reports/generated-hds --policy strict
```

Alternatively, leave the HDS starter empty and populate it with one or more
portable packages.

## Author A Floppy Image

The generated floppy starter contains one waveform and Sample Bank skeleton:

```bash
axklib create manifest floppy -o floppy.json
```

Place `tone.wav` next to the manifest or change the relative path, edit the
sampler-facing names, then create and validate the image:

```bash
axklib create floppy floppy.json -o authored.ima
axklib info authored.ima
axklib validate authored.ima -o reports/authored-floppy --policy strict
```

The result uses the documented 1.44 MB Yamaha FAT12 profile. See [FAT12 Floppy
Images](floppy.md) for geometry and filename rules.

## Author An ISO Image

The ISO starter is an empty staging volume:

```bash
axklib create manifest iso -o cdrom.json
axklib create iso cdrom.json -o staging.iso
```

Populate it with a package, or add authored waveform and Sample Bank entries to
`cdrom.json` before creation. A package import into the existing staging volume
uses its exact group, volume, and raw folder identifiers:

```bash
axklib package plan-import staging.iso source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"group":"NEW GROUP","volume":"NEW VOLUME","raw_group":"46DEF120","raw_volume":"F001"}' \
  --format json

axklib package import staging.iso source-volume.axkvol \
  --destination '{"package":0,"root":0,"partition":0,"group":"NEW GROUP","volume":"NEW VOLUME","raw_group":"46DEF120","raw_volume":"F001"}' \
  -o populated.iso --format json
```

For direct audio authoring, use the complete manifest in [Create A Hand-Authored
CD-ROM ISO](write.md#create-a-hand-authored-cd-rom-iso). For moving an existing
floppy object set, use [Convert A Floppy To An
ISO](write.md#convert-a-floppy-to-an-iso).

## Alter An Existing HDS Image

Generate a starter transaction, replace its placeholder names, and plan it
without an output path:

```bash
axklib alter manifest -o transaction.json
axklib alter hds source.hds transaction.json --pretty
```

After reviewing the plan, publish to a different path:

```bash
axklib alter hds source.hds transaction.json -o altered.hds --pretty
axklib validate altered.hds -o reports/altered --policy strict
```

See [Alteration Manifest](write.md#alteration-manifest) for supported operations
and dependency-safe ordering.

## Next References

- [CLI Reference](cli.md) lists every public command family and its safety behavior.
- [Writer And Alteration](write.md) defines image manifest fields and writer profiles.
- [Portable Object Packages](portable-packages.md) defines roots, destinations, conflicts, and reuse.
- [Supported Media Profiles](media.md) distinguishes SFS, Yamaha FAT12, and Yamaha ISO9660 boundaries.
