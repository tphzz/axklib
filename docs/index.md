# axklib

axklib is a C++23 library, stable C ABI, and CLI11 command-line application for
Yamaha A3000, A4000, and A5000 disk images and sampler objects.

It reads SFS HDA/HDS images, FAT12 floppy images, ISO9660 sample CD-ROMs, and
standalone sampler objects. It can inventory object relationships, export exact
waveforms and rendered stereo audio, create fresh HDS images, and apply ordered
changes to existing images.

## Build

```bash
export VCPKG_ROOT=/path/to/vcpkg
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

See [C++ and CLI usage](axklib/typical-usage.md), the [C++ API](axklib/cpp-api.md),
or the stable [C API](axklib/c-api.md).
