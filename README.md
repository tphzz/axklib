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

On Windows PowerShell, use an out-of-tree build directory so the source checkout
stays clean:

```powershell
$env:VCPKG_ROOT = Join-Path $HOME "vcpkg"
$buildDir = Join-Path $env:TEMP "axklib-build\debug"
cmake --preset debug -B $buildDir
cmake --build $buildDir --parallel
ctest --test-dir $buildDir --output-on-failure
```

The installed CMake targets are `axklib::core` for read-only C++ workflows,
`axklib::audio` for audio import and image writing, and `axklib::c` for the
full stable C ABI. Link `axklib::audio` when using the C++ writer or alteration
APIs; it brings in `axklib::core`. The C header is `axklib/c/axk.h`;
pkg-config consumers can use the `axklib` module.

`axklib::core` and `axklib::audio` are always static libraries, independent of
`BUILD_SHARED_LIBS`. `axklib::c` is the only shared library produced by the
project. Official builds use the `*-axk` overlay triplets under
`cpp/cmake/triplets` so third-party native dependencies are static as well.

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
