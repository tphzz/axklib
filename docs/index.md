# axklib

axklib provides **axkdeck**, a cross-platform desktop workspace for Yamaha
A3000, A4000, and A5000 sampler disks, plus a self-contained CLI and a C++
library for source integration.

## Start With axkdeck

Axkdeck can browse and audition Programs, Sample Banks, Samples, Wave Data, and
Sequences across supported hard-disk, floppy, CD-ROM, and A3K archive formats.
It can import audio and portable packages, organize Sample relationships,
export audio and SFZ instruments, and create or alter supported sampler images.
Planning views expose name conflicts, Program slots, storage use, and filesystem
record capacity before a write is committed.

Axkdeck validates SFS allocation metadata when an image is opened. Images with
unsafe allocation remain available for browsing and export, while alteration is
disabled and the Image integrity dialog explains the blocking issues. Axkdeck is
experimental software; always keep a backup before changing a disk image. For
the one supported malformed extent byte-total condition, the dialog can produce
a separately validated repaired copy without changing the source image.

[Download axkdeck](https://github.com/tphzz/axklib/releases)

On Windows, axkdeck requires Microsoft Edge WebView2 Evergreen Runtime version
111 or newer. The interactive NSIS installer reports a missing or outdated
runtime and asks before downloading or updating it from Microsoft. Silent `/S`
installations perform the prerequisite step without a prompt. The installer
uses the online Evergreen bootstrapper rather than bundling a fixed runtime and
leaves newer installed versions in place.

## Other Interfaces

- The [CLI reference](axklib/cli.md) covers scripted and batch workflows.
- [C++ and CLI usage](axklib/typical-usage.md) provides practical examples.
- The [C++ API](axklib/cpp-api.md) documents the source library.
- The [OpenAPI reference](axklib/openapi.md) renders the complete authenticated
  server contract used by axkdeck.

GitHub releases contain axkdeck installers and self-contained CLI archives for
the supported platforms. C++ consumers build the library from source and use
its installed CMake target, `axklib::axklib`; no prebuilt SDK archive is
published.

## Build From Source

```bash
git submodule update --init --recursive
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset release
cmake --build --preset release
cmake --install build/native/release --prefix ./axklib-install
```

The native build requires CMake 3.22.1 or newer, Ninja, Git, and a compiler with
C++23 support. Installed public library headers compile as C++17.
