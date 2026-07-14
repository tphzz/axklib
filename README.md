# axklib

axklib is native tooling for Yamaha A3000, A4000, and A5000 disk images and
sampler objects. It provides a C++17 shared SDK and a separate self-contained
command-line application backed by a C++23 implementation.

The project reads SFS HDA/HDS images, Yamaha-supported FAT12 floppy and ISO9660
sample CD-ROM profiles, and standalone sampler objects. It can inspect and
validate media, export exact waveform audio and rendered SFZ instruments,
transfer dependency-complete object packages, create supported HDS/floppy/ISO
images, and apply ordered changes to existing HDS images.

Documentation is published at <https://tphzz.github.io/axklib/>.

## Native Build

Building requires CMake 3.28 or newer, Ninja, Git, and a compiler with C++23
support. Dependencies and the vcpkg tool are pinned by the repository.

```bash
git clone --recurse-submodules https://github.com/tphzz/axklib.git
cd axklib
```

On Linux or macOS:

```bash
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

For an existing checkout, initialize or update the pinned vcpkg submodule with
`git submodule update --init --recursive` before configuring.

On Windows PowerShell, use an out-of-tree build directory:

```powershell
.\external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$buildDir = Join-Path $env:TEMP "axklib-build\debug"
cmake --preset debug -B $buildDir
cmake --build $buildDir --parallel
ctest --test-dir $buildDir --output-on-failure
```

Installed CMake consumers link the single target `axklib::axklib`. Its headers
compile as C++17; parser, allocation, codec, JSON, and C++23 implementation types
are private. The shared library embeds its non-system dependencies statically.
See [C++ and CLI usage](docs/axklib/typical-usage.md) for a complete consumer
example.

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

Install the shared SDK, CLI, headers, licenses, and CMake package metadata from
a release build:

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/native/release --prefix ./axklib-install
```

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

## Documentation

The documentation is an independent, locked environment and does not depend on
the native build. Install [uv](https://docs.astral.sh/uv/) and Node.js 24, then
build the MkDocs site from the repository root. The pinned Mermaid CLI renders
diagram fences to static SVG during the build:

```bash
uv --project docs sync --locked
npm ci
cmake -E make_directory build/docs/site
PATH="$PWD/node_modules/.bin:$PATH" \
  uv --project docs run mkdocs build --strict --config-file mkdocs.yml
```

The generated site is written to `build/docs/site`. For a local preview:

```bash
uv --project docs run python -m http.server --directory build/docs/site 8000
```

Start with the [typical usage guide](docs/axklib/typical-usage.md) for image
inspection, extraction, package transfer, image authoring, and alteration. The
[CLI reference](docs/axklib/cli.md) lists the full command surface and safety
behavior.

## License

This project is licensed under the Mozilla Public License 2.0.
