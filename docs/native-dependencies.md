# Native dependency policy

The C++ library uses a pinned vcpkg manifest. The manifest baseline and package
versions are part of the reproducible native build contract.

Clone with `git submodule update --init --recursive`, then bootstrap
`external/vcpkg` using its platform script. To update vcpkg, review one explicit
submodule commit, set `builtin-baseline` to that same commit, regenerate SBOMs,
and run the complete architecture matrix. A manifest-only or submodule-only
version change is invalid.

| Package | Purpose | Upstream license |
| --- | --- | --- |
| CLI11 | Native command-line argument parsing | BSD-3-Clause |
| hash-library v8 | CLI pooled-export SHA-1 compatibility identifiers | Zlib |
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
