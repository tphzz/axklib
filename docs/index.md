# axklib

axklib is a C++17 shared SDK and a separate native command-line application for
Yamaha A3000, A4000, and A5000 disk images and sampler objects. Its internal
engine is implemented in C++23.

It reads SFS HDA/HDS images, FAT12 floppy images, ISO9660 sample CD-ROMs, and
standalone sampler objects. It can inventory object relationships, export exact
waveforms and rendered stereo audio, create fresh HDS images, and apply ordered
changes to existing images. It can also build narrow FAT12 floppy and primary
ISO9660 image profiles for file-based testing and transfer workflows.

## Build

```bash
git submodule update --init --recursive
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset release
cmake --build --preset release
cmake --install build/native/release --prefix ./axklib-sdk
```

Installed consumers use `find_package(axklib CONFIG REQUIRED)`. The native CLI
is installed as `axklib`.

## First Command

```bash
axklib info HD00_512_example.hds
```

See [C++ and CLI usage](axklib/typical-usage.md) or the
[C++ API](axklib/cpp-api.md).
