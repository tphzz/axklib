# Overview

The production distribution has four surfaces:

- `axklib::core`, the C++23 API for native applications.
- `axklib::audio`, the optional audio-import and image-writing C++ layer.
- `axklib::c`, the versioned C ABI for foreign-language and plugin hosts.
- `axklib`, the CLI11 application for interactive and automated workflows.
- Versioned JSON and CSV contracts emitted by the CLI.

Read operations cover object inventory, relationships, content trees,
validation, exact waveform extraction, stereo rendering, and SFZ output. Write
operations cover fresh images and ordered alteration transactions. A write is
planned and validated before the destination is replaced.

The SDK supports Linux, macOS, and Windows on x64 and arm64. Platform packages
contain headers, libraries, the CLI, C examples, CMake metadata, pkg-config
metadata where applicable, license texts, and an SPDX software bill of
materials.
