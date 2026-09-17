# Native dependency policy

The C++ library uses a pinned vcpkg manifest. The manifest baseline, package
versions, and source submodule revisions are part of the reproducible native
build contract.

Clone with `git submodule update --init --recursive`. This initializes both the
vcpkg tool under `external/vcpkg` and the FatFs source under `external/fatfs`.
Bootstrap vcpkg using its platform script.

To update vcpkg, review one explicit submodule commit, set `builtin-baseline` to
that same commit, regenerate SBOMs, and run the complete architecture matrix. A
manifest-only or submodule-only vcpkg version change is invalid.

FatFs is pinned to the `abbrev/fatfs` R0.16 commit
`30ca13c62615df0d2e9104ab41256985b96590c1c`. That repository is an unofficial
Git mirror, not the FatFs upstream. The overlay port verifies the exact `ff.c`,
`ff.h`, `diskio.h`, and `LICENSE.txt` bytes against hashes derived from the
official R0.16 archive before building. Updating FatFs requires comparison with
an official release archive, updating the pinned submodule commit and overlay
hashes, incrementing the overlay port revision, and running the complete native
architecture matrix.

| Package | Purpose | Upstream license |
| --- | --- | --- |
| CLI11 | Native command-line argument parsing | BSD-3-Clause |
| hash-library v8 | CLI pooled-export SHA-1 compatibility identifiers | Zlib |
| ICU | Unicode-aware natural filename ordering in application storage browsers | ICU |
| nlohmann/json | Versioned JSON manifests and reports | MIT |
| FatFs | FAT12 image authoring | BSD-1-Clause |
| libsndfile | WAV, AIFF, and FLAC decoding | LGPL-2.1-or-later |
| libFLAC | FLAC codec used by libsndfile | BSD-3-Clause |
| libogg | Ogg container support used by libsndfile | BSD-3-Clause |
| libvorbis | Vorbis codec used by libsndfile | BSD-3-Clause |
| Opus | Opus codec used by libsndfile | BSD-3-Clause |
| libsoxr | Very-high-quality resampling | LGPL-2.1-or-later |
| utfcpp 4.1.1 | Internal UTF-8 validation and checked UTF-16 conversion | BSL-1.0 |
| GoogleTest | Native tests only | BSD-3-Clause |

The audio quantizer and its PCG32 generator are implemented inside axklib and
add no runtime dependency. The generator uses the specified PCG-XSH-RR
64-bit-state/32-bit-output transition with axklib-owned seed and stream IDs.
Those constants define the public `axk-tpdf-pcg32-v1` conversion policy; they
are not copied NumPy state and are not sampler-format constants. Unsigned
64-bit overflow is intentional and defined by C++. The transition multiplier
is `6364136223846793005`, as specified by the upstream
[PCG family paper](https://www.pcg-random.org/pdf/toms-oneill-pcg-family-v1.02.pdf).
Axklib seeds both streams with `0x41584b`; their stream selectors are
`0x41584b01` and `0x41584b02`. These three values spell the project identifier
and provide stable, independent stream identities; they have no external format
meaning.

Packagers must audit the exact copyright files installed by vcpkg and satisfy
the applicable license terms. In particular, static distribution must not
obscure the recipient's rights for LGPL-covered libraries. This table is an
engineering summary, not legal advice and not a substitute for the installed
license texts.

Compact GitHub CLI archives move the project license to `LICENSE` and dependency
copyright files to `licenses/` at the archive root. They omit the normal CMake
`share/` hierarchy, but this layout change does not remove any required legal
notice. Source consumers and system packaging through `cmake --install` retain
the conventional `share/licenses/` layout.

Official release builds use the overlay triplets under
`library/cmake/triplets`. They select static, release-only dependency libraries.
Pass the matching `*-axk` triplet through `VCPKG_TARGET_TRIPLET`. This linkage
policy does not change or waive any dependency license obligation.

The project linkage boundary is fixed across platforms:

- `axklib::axklib` is the installed C++17 shared SDK and embeds the static engine.
- The CLI embeds the engine and native dependencies directly; it does not load
  the shared SDK.

`BUILD_SHARED_LIBS` does not change these target types. This avoids application
packages that depend on private axklib or codec libraries beside the executable.
The native library does not link or invoke a scripting runtime.

## Local Linux build with Clang 18

The standard `release` preset selects Clang 18 and libc++ on a clean Linux
configuration, using `x64-linux-axk` or `arm64-linux-axk` for both the native
project and target dependencies. This matches Linux CI's compiler major version;
macOS uses Apple Clang. Local validation does not replace the platform builds.

Local and workflow Clang builds share the project-target warning policy in
`library/cmake/AxkWarnings.cmake`, including `-Wshadow` and
`-Wshadow-uncaptured-local`. These catch variable shadowing, including inside
lambdas that do not capture the hidden local. Warnings remain errors by default;
the policy applies to maintained code and tests, not dependency targets.
Constructor parameters that simply initialize equally named fields are allowed.
MSVC retains `/W4 /WX`; the compilers' diagnostic coverage is not identical.

On Debian 13, the compiler and tools can be installed alongside Clang 19:

```bash
sudo apt-get install --no-install-recommends clang-18 clang-tools-18
```

Run these commands from the axklib source root:

```bash
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --parallel 1
```

The output remains `build/native/release`, so existing CLI, server and desktop
commands consume the Clang 18 build.
Before switching an existing release directory between compilers, standard
libraries, or target triplets, move aside or delete that disposable directory,
including its `vcpkg_installed` subdirectory, and configure again with the
intended preset.
Do not reuse Clang 19/GCC objects or libstdc++ target dependencies in this build.

The `release` preset retains an existing cache's selected toolchain; merely
reconfiguring a Clang 19 directory will not switch it. Debug and sanitizer build
directories are configured independently.

The libc++ development packages are also required. Debian's libc++ 18 and 19
packages conflict; installing `libc++-18-dev libc++abi-18-dev` can remove the
installed version 19 packages. Do not silently replace those host libraries.
Clang 18 with an installed libc++ 19 is a useful local check, but not identical
to CI's standard library. For closer parity use an isolated environment with
the CI distribution and libc++ version, and a separate build/dependency cache.
