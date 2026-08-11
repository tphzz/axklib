# Overview

The repository has three user-facing surfaces:

- **axkdeck**, the cross-platform desktop application for browsing, auditioning,
  importing, exporting, and authoring sampler media;
- **`axklib`**, a self-contained command-line application for interactive and
  automated workflows; and
- **`axklib::axklib`**, the C++17 library interface built and installed from the
  source tree.

Axkdeck bundles a local `axklib-server` sidecar. The server exposes the same
application services through authenticated JSON REST and WebSocket contracts,
but is not a separate end-user download. The CLI also emits versioned JSON and
CSV contracts for automation.

Read operations cover object inventory, relationships, content trees,
validation, exact Wave Data extraction, stereo rendering, and SFZ output.
Write operations cover fresh images, package transfer, and ordered alteration
transactions. A write is planned and validated before the destination is
replaced.

## Distribution

GitHub releases contain axkdeck installers and self-contained CLI archives for
Linux, macOS, and Windows on x64 and ARM64 where supported. The server and its
desktop SBOM are distributed inside axkdeck installers.

The C++ library is distributed as source. Consumers build it with the pinned
repository dependencies and may install the `axklib::axklib` CMake package into
their own prefix. This avoids publishing compiler- and standard-library-specific
SDK archives while preserving the normal CMake consumer workflow.

Continue with [C++ and CLI usage](typical-usage.md), the
[C++ API](cpp-api.md), or the [CLI reference](cli.md). Server clients can use
the rendered [OpenAPI reference](openapi.md).
