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
the applicable license terms. In particular, distribution must not obscure the
recipient's rights for LGPL-covered libraries. Applications may choose dynamic
linkage where that simplifies compliance. This table is an engineering summary,
not legal advice and not a substitute for the installed license texts.

Official release builds use the overlay triplets under
`cpp/cmake/triplets`. They select static dependency libraries and remap build
roots in compiler-provided file names so SDK and desktop artifacts do not expose
host paths. Set `AXK_PATH_REMAP_FROM` to the source checkout root and pass the
matching `*-axk` triplet through `VCPKG_TARGET_TRIPLET`. This reproducibility
mechanism does not change or waive any dependency license obligation.
