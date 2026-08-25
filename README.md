# axklib

axklib is a cross-platform toolkit for working with Yamaha A3000, A4000, and
A5000 sampler disks. Its main application is **axkdeck**, a desktop workspace
for browsing, auditioning, organizing, importing, exporting, and basic authoring
of sampler media.

The repository also provides a self-contained `axklib` command-line tool and
the C++ library source used by both applications.

- [Download axkdeck or the CLI](https://github.com/tphzz/axklib/releases)
- [Read the documentation](https://tphzz.github.io/axklib/)
- [Browse the CLI reference](https://tphzz.github.io/axklib/axklib/cli.html)
- [Use the C++ library](https://tphzz.github.io/axklib/axklib/cpp-api.html)
- [Browse the OpenAPI reference](https://tphzz.github.io/axklib/axklib/openapi.html)

## axkdeck

axkdeck presents sampler media as Programs, Sample Banks, Samples, Wave Data,
and Sequences using the same object hierarchy across hard disks, floppies, and
CD-ROMs.

<img width="1802" height="1156" alt="image" src="https://github.com/user-attachments/assets/dba48def-6e9a-46e2-97b6-49e96cc3676a" />


### Browse And Audition

- Open SFS HDA/HDS images, FAT12 floppy images, ISO9660 sample CD-ROMs, A3K
  `.a3k` volume archives, and supported standalone sampler objects 
  (e.g. from extracted floppy images).
- Browse partitions, volumes, Programs (`PROG`), Sample Banks (`SBAC`), Samples
  (`SBNK`), Wave Data (`SMPL`), and Sequences.
- Inspect relationships and sampler parameters without losing the visible
  parent and volume context.
- Audition individual Samples, Sample Banks, and their playable audio directly
  from the desktop.

### Import And Organize

- Import WAV/FLAC files as Samples or collect them into a new Sample Bank in one
  operation, including compatible WAV sampler metadata (SMPL chunks) and loops.
- Create Sample Banks from selected Samples or relink existing Samples to a
  chosen Sample Bank.
- Import portable Program or volume packages with dependency planning, Program
  slot suggestions, conflict checking, and SFS record-capacity feedback.
- Batch-import volume packages into a partition using their placement hints.
- Create and rename volumes / rename sampler objects.
- Clean up unused wave data.
- Generate simple Programs for otherwise unreferenced Sample Banks and Samples
  so they can be played immediately on compatible A-series instruments.

### Export And Author Media

- Export selected Samples as mono or interleaved stereo WAV files, selected Wave
  Data as mono WAV files, rendered SFZ instruments (needs more work), individual
  object packages, and dependency-complete volume packages.
- Batch-export every volume in a partition as packages or as per-volume floppy
  sets.
- Create formatted multi-partition HDS images and 1.44 MB Yamaha-compatible
  floppy images.
- Export Yamaha-compatible multi-floppy sets and ISO9660 CD-ROM images.
- Insert, delete, rename, and repair supported image content through planned,
  transactional alterations with rollback protection.

See the [axkdeck development guide](apps/axkdeck/README.md) when building the
desktop application from source.

## Downloads

The [GitHub releases](https://github.com/tphzz/axklib/releases) provide:

- axkdeck DEB and RPM packages for Linux x64 and ARM64;
- axkdeck NSIS installers for Windows x64 and ARM64;
- one universal axkdeck DMG for Apple silicon and Intel macOS; and
- self-contained CLI archives for Linux x64/ARM64, Windows x64/ARM64, and
  universal macOS.

The release does not include a prebuilt C++ SDK archive. Library consumers use
the source tree and its CMake package, keeping the compiler, standard library,
and dependency choices under their own control. The local `axklib-server` is an
axkdeck sidecar and is not published as a standalone download.

Windows installers require Microsoft Edge WebView2 Evergreen Runtime version
111 or newer. An interactive installation asks before downloading or updating
an insufficient runtime from Microsoft; silent `/S` installations perform that
prerequisite step without a prompt. The installer does not bundle a fixed
WebView2 runtime and does not replace a newer installed version.

Linux packages use the system WebKitGTK runtime and LLVM 18 C++ runtime. The
RPM declares `webkit2gtk4.1`, `gtk3`, `libcxx`, and `llvm-libunwind`. The DEB
declares `libwebkit2gtk-4.1-0`, `libgtk-3-0`, `libc++1-18`, `libc++abi1-18`,
and `libunwind-18`. Debian or Ubuntu releases that do not provide the LLVM 18
runtime packages in their standard repositories require an appropriate LLVM
package source before the DEB can be installed. An older generic `libc++1`
package is not a compatible substitute.

## Command Line

The CLI exposes the same image and object operations for scripts and batch
workflows. For example:

```bash
axklib info HD00_512_example.hds
axklib validate -o validation HD00_512_example.hds
axklib extract wav file -o wav HD00_512_example.hds
```

Use `axklib --help` to discover commands. The
[CLI reference](https://tphzz.github.io/axklib/axklib/cli.html) documents the
complete command surface, output contracts, and write-safety behavior.

## C++ Library

Library users build from the repository source. CMake 3.22.1 or newer, Ninja,
Git, and a compiler with C++23 support are required to build the implementation;
installed public headers compile as C++17.

```bash
git clone --recurse-submodules https://github.com/tphzz/axklib.git
cd axklib
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset release
cmake --build --preset release
cmake --install build/native/release --prefix ./axklib-install
```

Installed CMake consumers use:

```cmake
find_package(axklib CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE axklib::axklib)
```

The [C++ API](https://tphzz.github.io/axklib/axklib/cpp-api.html),
[usage guide](https://tphzz.github.io/axklib/axklib/typical-usage.html), and
[native dependency policy](https://tphzz.github.io/axklib/native-dependencies.html)
describe the supported interface and build contract.

## Documentation And API Contracts

The [documentation site](https://tphzz.github.io/axklib/) covers media formats,
sampler data structures, package transfer, image writing, compatibility, and
the public library and CLI contracts. The bundled server contract is available
as a rendered [OpenAPI reference](https://tphzz.github.io/axklib/axklib/openapi.html)
and as downloadable OpenAPI JSON from that page.

To build the documentation locally:

```bash
uv --project docs sync --locked
npm ci
PATH="$PWD/node_modules/.bin:$PATH" \
  uv --project docs run mkdocs build --strict --config-file mkdocs.yml
```

The generated site is written to `build/docs/site`.

## License

axklib is licensed under the Mozilla Public License 2.0. Third-party
dependencies retain their own licenses; see the
[native dependency policy](docs/native-dependencies.md).
