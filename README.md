# axklib

Yamaha A-series disk image and sampler object tooling.

Documentation is published at <https://tphzz.github.io/axklib/>.

## Native Build

axklib provides a C++17 shared SDK and a separate self-contained CLI. The
implementation uses C++23. Dependencies and the vcpkg tool are pinned by the
repository.

```bash
git submodule update --init --recursive
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

On Windows PowerShell, use an out-of-tree build directory so the source checkout
stays clean:

```powershell
git submodule update --init --recursive
.\external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$buildDir = Join-Path $env:TEMP "axklib-build\debug"
cmake --preset debug -B $buildDir
cmake --build $buildDir --parallel
ctest --test-dir $buildDir --output-on-failure
```

Installed CMake consumers link the single target `axklib::axklib`. Its headers
compile as C++17; parser, allocation, codec, JSON, and C++23 implementation types
are private. The shared library embeds its non-system dependencies statically.

The CLI lives under `apps/cli`, links the private engine statically, and does not
load the shared SDK. Official builds use the `*-axk` overlay triplets under
`library/cmake/triplets`.

The two source projects also configure independently:

```bash
cmake -S library -B build/library -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/external/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake -S apps/cli -B build/cli -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/external/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

The library-only manifest profile does not resolve CLI11, hash-library, or
GoogleTest when `BUILD_TESTING=OFF`.

## CLI

Show the native CLI help:

```bash
build/native/debug/apps/cli/axklib --help
```

Summarize an image, directory, or glob input:

```bash
build/native/debug/apps/cli/axklib info <image-or-directory>
```

Export waveform data:

```bash
build/native/debug/apps/cli/axklib extract wav file -o build/exports/wav <image-or-directory>
```

Write validation reports:

```bash
build/native/debug/apps/cli/axklib validate -o build/reports/validation <image-or-directory>
```

Create separate SDK and CLI archives:

```bash
cmake --preset release
cmake --build --preset release
cd build/native/release && cpack
```

The presets select axklib's static-dependency vcpkg triplet for the host
architecture. After changing triplets or updating an existing checkout, use
`cmake --fresh --preset release` once to discard the previous vcpkg selection.

The native GitHub workflow combines those components into one distribution per
target. Windows publishes a ZIP, Linux publishes a compressed tar archive, and
macOS publishes one universal ZIP containing both the self-contained CLI and
the shared SDK development files.

Maintainers publish tagged releases with the documented
[manual release procedure](RELEASING.md).

## License

This project is licensed under the Mozilla Public License 2.0.
