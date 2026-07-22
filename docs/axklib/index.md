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

The SDK and server support Linux, macOS, and Windows on x64 and arm64. GitHub
releases keep the native surfaces separate: SDK archives contain headers, the
shared library, CMake package metadata, and legal notices; CLI archives contain
the self-contained command and legal notices. The server and its desktop SBOM
are distributed inside axkdeck installers. A conventional CMake installation
still installs the selected component's `share/` metadata for system packagers.
