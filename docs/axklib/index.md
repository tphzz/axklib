# Overview

The production distribution has four surfaces:

- `axklib::axklib`, the C++17 shared SDK for native applications.
- `axklib`, a separate self-contained command-line application for interactive
  and automated workflows.
- `axklib-server`, a Crow-based JSON REST and WebSocket application for axkdeck
  and other authenticated clients.
- Versioned JSON and CSV contracts emitted by the CLI.

Read operations cover object inventory, relationships, content trees,
validation, exact waveform extraction, stereo rendering, and SFZ output. Write
operations cover fresh images and ordered alteration transactions. A write is
planned and validated before the destination is replaced.

The SDK and server support Linux, macOS, and Windows on x64 and arm64. Platform
packages contain headers, the shared library, the CLI, the server, its OpenAPI
contract, CMake package metadata, dependency license texts, and SPDX software
bills of materials.
