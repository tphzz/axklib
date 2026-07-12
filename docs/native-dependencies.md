# Native dependency policy

The C++ library uses a pinned vcpkg manifest. The manifest baseline and package
versions are part of the reproducible native build contract.

| Package | Purpose | Upstream license |
| --- | --- | --- |
| CLI11 | Native command-line argument parsing | BSD-3-Clause |
| nlohmann/json | Versioned JSON manifests and reports | MIT |
| libsndfile | Optional `axklib::audio` WAV, AIFF, and FLAC decoding | LGPL-2.1-or-later |
| libsoxr | Optional `axklib::audio` very-high-quality resampling | LGPL-2.1-or-later |
| GoogleTest | Native tests only | BSD-3-Clause |

Packagers must audit the exact copyright files installed by vcpkg and satisfy
the applicable license terms. In particular, static distribution must not
obscure the recipient's rights for LGPL-covered libraries. This table is an
engineering summary, not legal advice and not a substitute for the installed
license texts.

Official release builds use the overlay triplets under
`cpp/cmake/triplets`. They select static dependency libraries and remap build
roots in compiler-provided file names so SDK and desktop artifacts do not expose
host paths. Set `AXK_PATH_REMAP_FROM` to the source checkout root and pass the
matching `*-axk` triplet through `VCPKG_TARGET_TRIPLET`. This reproducibility
mechanism does not change or waive any dependency license obligation.

The project linkage boundary is fixed across platforms:

- `axklib::core` and `axklib::audio` are static archives.
- The CLI and desktop application embed those archives and their native
  dependencies.
- `axklib::c` is the sole shared library and embeds the same static engine.

`BUILD_SHARED_LIBS` does not change these target types. This avoids application
packages that depend on private axklib or codec libraries beside the executable.
