# axklib

axklib provides **axkdeck**, a cross-platform desktop workspace for Yamaha
A3000, A4000, and A5000 sampler disks, with additional EX5 and SU700 filesystem
support, plus a self-contained CLI and a C++ library for source integration.

## Start With axkdeck

**Device mode** browses A-series Programs, Sample Banks, Samples, Wave Data,
and Sequences across supported hard-disk, floppy, CD-ROM, and A3K archive
formats. Audition Samples, import WAV/FLAC/AIFF audio, portable packages or
A-series floppies and unpacked disk folders, and organize Sample relationships.
Export audio, [supported SFZ instruments](axklib/sfz.md), packages and authored
A-series media. Planning views expose name conflicts, Program slots, storage
use, and filesystem record capacity before a write is committed.

**Files mode** browses and exports directories and files, including EX5 and
SU700 media. Supported writable SFS, FAT16 and EX5 roots allow directory
creation, rename, deletion and recursive import. FAT16 destinations offer
**File / Contents** when importing floppy images; recognized SU700 hard disks
can import a complete SU700 floppy into a new volume. EX5 and SU700 support
does not include sound editing, audition, A-series package conversion or
formatted-image creation. Raw file edits do not repair sampler-object
relationships. See [supported media profiles](axklib/media.md) for the
compatibility boundaries.

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
