# axklib

Yamaha A-series disk image and sampler object tooling.

Documentation is published at <https://tphzz.github.io/axklib/>.

## Native Build

axklib is a C++23 library, a versioned C SDK, and a CLI11 command-line tool.
Dependencies are pinned by `vcpkg.json`.

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The installed CMake targets are `axklib::core` for read-only C++ workflows,
`axklib::audio` for audio import and image writing, and `axklib::c` for the
full stable C ABI. Link `axklib::audio` when using the C++ writer or alteration
APIs; it brings in `axklib::core`. The C header is `axklib/c/axk.h`;
pkg-config consumers can use the `axklib` module.

## CLI

Show the native CLI help:

```bash
build/native/debug/cpp/axklib --help
```

Summarize an image, directory, or glob input:

```bash
build/native/debug/cpp/axklib info <image-or-directory>
```

Export waveform data:

```bash
build/native/debug/cpp/axklib extract wav file -o build/exports/wav <image-or-directory>
```

Write validation reports:

```bash
build/native/debug/cpp/axklib validate -o build/reports/validation <image-or-directory>
```

Create installable SDK archives:

```bash
cmake --preset release
cmake --build --preset release
cd build/native/release && cpack
```

The compatibility oracle is maintained separately under `oracle/` and is not
part of native SDK or application packages.

## License

This project is licensed under the Mozilla Public License 2.0.
