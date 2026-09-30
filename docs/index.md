# axklib

axklib provides **axkdeck**, a cross-platform desktop workspace for Yamaha
A3000, A4000, and A5000 sampler disks, with additional EX5 and SU700 filesystem
support, plus a self-contained CLI and a C++ library for source integration.

## Start With axkdeck

**Device mode** browses A-series Programs, Sample Banks, Samples, Wave Data,
and Sequences across supported hard-disk, floppy, CD-ROM, and A3K archive
formats. Audition Samples and Sample Banks, edit supported A3000 and A4000/A5000
Sample parameters, and apply bank-wide overrides without rewriting member
Sample settings. Explicit [format conversion](axklib/sample-formats.md) checks
that settings can be represented in the target generation.

Import WAV/FLAC/AIFF audio in `a3k` or `a4k/a5k` format, portable packages,
A-series floppies and unpacked disk folders, and organize Sample relationships.
Floppy and package source pickers remember their last-used directories.
Planning views expose name conflicts, Program slots, storage use and filesystem
record capacity before a write is committed. [Sampler capacity checks](axklib/volume-capacity.md)
also compare the resulting volume with the selected A3000 V2 or A4000/A5000
parameter-memory and object-slot limits, blocking imports that exceed them.
These checks cover a clean full-volume load, not waveform RAM or merging into
existing sampler memory.

Export audio, [supported SFZ instruments](axklib/sfz.md), packages and authored
A-series media, including multi-floppy sets and CD-ROM images. Create formatted
HDS images up to 8 GiB with valid partition combinations, smaller 128/256 MiB
test images, or blank 1.44 MB floppies; see [image authoring](axklib/write.md)
for size and compatibility limits.

**Files mode** browses and exports directories and files, including EX5 and
SU700 media. Supported writable SFS, FAT16 and EX5 roots allow directory
creation, rename, deletion and recursive import. FAT16 destinations offer
**File / Contents** when importing floppy images; recognized SU700 hard disks
can import a complete SU700 floppy into a new volume. EX5 and SU700 support
does not include sound editing, audition, A-series package conversion or
formatted-image creation. Raw file edits do not repair sampler-object
relationships. See [supported media profiles](axklib/media.md) for the
compatibility boundaries.

Preferences persist the **Preferred A-Series generation** used to preselect audio
import and sampler load targets; each operation can override the suggestion.
The desktop **Logs...** window shows Application and bundled local-server logs,
with source, minimum-level and search filters, text-selection copying and log
saving. It does not retrieve remote-server logs.

## Testing And Backups

Read and write support has been exercised on Yamaha A4000 and A5000 hardware,
including loading and auditioning generated media and save/reload checks.
Automated testing also covers a broad collection of real hard-disk images,
floppy images (`.ima`), unpacked floppy directories, and CD-ROM images.
Compatibility is documented by media type and operation.

Axkdeck is actively developed pre-release software. Keep an untouched backup
before changing an image, and work on copies of irreplaceable media.
Axkdeck validates SFS allocation metadata when an image is opened. Images with
unsafe allocation remain available for browsing and export, while alteration is
disabled and the Image integrity dialog explains the blocking issues. For the
one supported malformed extent byte-total condition, the dialog can produce a
separately validated repaired copy without changing the source image.

[Download axkdeck](https://github.com/tphzz/axklib/releases)

On Windows, axkdeck requires Microsoft Edge WebView2 Evergreen Runtime version
111 or newer. The interactive NSIS installer reports a missing or outdated
runtime and asks before downloading or updating it from Microsoft. Silent `/S`
installations perform the prerequisite step without a prompt. The installer
uses the online Evergreen bootstrapper rather than bundling a fixed runtime and
leaves newer installed versions in place.

## Local And Remote Use

`axklib` handles media formats and image operations, `axklib-server` exposes
those capabilities through an API, and axkdeck provides the desktop interface.
Normal desktop use includes a bundled local server. This separation also allows
axkdeck to work with images on a separately configured remote host, such as a
Raspberry Pi used for PiSCSI, without first copying whole images to the desktop.

This is remote image access, not PiSCSI hardware control or live editing of disks
mounted by a sampler. See [server configuration](axklib/server.md#configuration)
and the [Raspberry Pi deployment profile](axklib/server.md#low-concurrency-deployment-profile)
for authentication, network security and resource requirements.

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
