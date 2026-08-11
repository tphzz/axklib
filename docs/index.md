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

[Download axkdeck](https://github.com/tphzz/axklib/releases)

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
