# axklib

axklib is a cross-platform toolkit for Yamaha sampler media, focused on A3000,
A4000, and A5000 disks, with additional EX5 and SU700 filesystem support.
Its main application is **axkdeck**, a desktop workspace for browsing,
auditioning, organizing, importing, exporting, and authoring supported media.

The repository also provides a self-contained `axklib` command-line tool and
the C++ library source used by both applications.

- [Download axkdeck or the CLI](https://github.com/tphzz/axklib/releases)
- [Read the documentation](https://tphzz.github.io/axklib/)
- [Browse the CLI reference](https://tphzz.github.io/axklib/axklib/cli.html)
- [Use the C++ library](https://tphzz.github.io/axklib/axklib/cpp-api.html)
- [Browse the OpenAPI reference](https://tphzz.github.io/axklib/axklib/openapi.html)

## axkdeck

**Device mode** presents A-series media as Programs, Sample Banks, Samples,
Wave Data, and Sequences across supported hard disks, floppies, CD-ROMs, and
archives. Inspect relationships and audition Samples in their volume context.

**Files mode** exposes the directories and files stored inside an image,
including EX5 and SU700 media. On A-series images, navigate between an object
and its corresponding file through the inspector.


<p align="center">
  <img width="24%" alt="programs" src="https://github.com/user-attachments/assets/2dcbd887-dd10-43f4-b5c6-bf3c141dc433" />
  <img width="24%" alt="sample_banks" src="https://github.com/user-attachments/assets/98ef1290-f7c1-41d1-a6a6-cda5abe45eea" />
  <img width="24%" alt="samples" src="https://github.com/user-attachments/assets/9812b577-c530-4fcc-b320-6bd7764c60ff" />
  <img width="24%" alt="wave_data" src="https://github.com/user-attachments/assets/930cb33c-7ebe-4744-8cd6-6375856f7915" />
  <br/>
  <img width="24%" alt="files" src="https://github.com/user-attachments/assets/6e54d238-9040-4686-93cc-ca1d44ec85bc" />
  <img width="24%" alt="import_audio" src="https://github.com/user-attachments/assets/6a14699c-da72-477e-91fa-7278e3c5b315" />
  <img width="24%" alt="import_floppy" src="https://github.com/user-attachments/assets/8f14499c-7be7-4fae-b88a-dc835f52507b" />
  <img width="24%" alt="import_packages" src="https://github.com/user-attachments/assets/250f0b05-fbeb-4f26-b24b-301a5f9b6f4a" />
</p>

### Supported Media

| Media | Access |
| --- | --- |
| A-series SFS HDA/HDS, FAT12 floppies, ISO9660 sample CD-ROMs, A3K archives, and sampler object files/folders | Object browsing, audition and export; supported object and filesystem edits on writable SFS images. |
| EX5 hard-disk and MO/removable images, plus FAT12 floppies | Directory browsing and raw file export; supported filesystem edits on writable HD/removable images. |
| SU700 SFS hard disks and FAT12 floppies | Directory browsing and raw file export; supported filesystem edits and complete-floppy import on recognized writable SU700 hard disks. |
| Standard FAT16 volumes and primary-MBR FAT16 disks | Directory browsing, raw file export and supported filesystem edits. |

EX5 and SU700 access does not include sound editing, audition, or conversion
to A-series object packages. Readable media are not necessarily writable; see
the [supported media profiles](docs/axklib/media.md) for format-specific limits.

Read and write support has been exercised on Yamaha A4000 and A5000 hardware,
including loading and auditioning generated media and save/reload checks.
Automated testing also covers a broad collection of real hard-disk images,
floppy images (`.ima`), unpacked floppy directories, and CD-ROM images.
Compatibility is documented by media type and operation.

Axkdeck is actively developed pre-release software. Keep an untouched backup
before changing an image, and work on copies of irreplaceable media.
Writes require a writable source and safe allocation metadata; images with
unsafe allocation remain available for browsing and export of readable content.

### Browse And Audition

- Browse partitions, volumes, Programs (`PROG`), Sample Banks (`SBAC`), Samples
  (`SBNK`), Wave Data (`SMPL`), and Sequences on A-series media.
- Open unpacked A-series floppy folders and attach companion images or folders
  when a disk set spans multiple floppies.
- Inspect relationships and sampler parameters without losing the visible
  parent and volume context.
- Audition individual Samples, Sample Banks, and their playable audio directly
  from the desktop, with waveform playback windows and loop boundaries visible.
- Edit supported A3000 and A4000/A5000 Samples in six device-specific parameter tabs,
  with draggable playback/loop markers, session-only drafts, undo/redo and
  explicit Save. Draft audition previews bounds, pitch, level and pan;
  it does not emulate sampler filters, envelopes or effects.
- Edit Sample Bank overrides without rewriting member Sample parameters;
  settings without a bank override use each member's own values.
- Edit key/velocity ranges graphically in the shared Mapping Editor for Samples,
  bank members and Program assignments. Hold a keyboard key to audition matching
  layers at the chosen velocity without changing their roots or selection.
- Edit supported native A3000 and A4000/A5000 Programs: assignment Easy Edit,
  effect routing and parameters, setup, controllers and StepWave. Program
  drafts use undo/redo and explicit Save, without changing the stored format.
  Effect parameters use their stored numeric ranges; the editor does not
  emulate sampler effects. See [Program parameters](docs/axklib/program-parameters.md).
- Explicitly convert supported Samples and Sample Banks between `a3k` and
  `a4k/a5k` formats, with checks that reject settings that cannot be represented.
  Converting a bank does not convert its member Samples. See
  [Sample and Sample Bank formats](docs/axklib/sample-formats.md).
- Inspect Program `a3k`/`a4k/a5k` badges and explicitly convert one Program
  when its settings can be retained, without changing its linked objects or
  System Files. See [Program formats](docs/axklib/program-formats.md).

### Import And Organize

- Import WAV, FLAC, and AIFF files as Samples or collect them into a new Sample
  Bank, choosing `a3k` or `a4k/a5k` format and retaining compatible WAV sampler
  metadata and loops.
- Import A-series floppy images or unpacked disk folders directly into a
  writable A-series SFS image, including companion sets, without intermediate packages.
- Create Sample Banks from selected Samples or relink existing Samples to a
  chosen Sample Bank. A new bank's suggested format follows the highest
  generation in the selection and can be overridden.
- Import portable Program or volume packages with dependency planning, Program
  slot suggestions, conflict checking, and SFS record-capacity feedback.
- Batch-import volume packages into a partition using their placement hints.
- Create and rename volumes, rename sampler objects, and clean up unused Wave Data.
- Generate simple Programs for otherwise unreferenced Sample Banks and Samples
  so they can be played immediately on compatible A-series instruments.

Inspect each volume's parameter-memory and shared object-slot requirements for
A3000 V2 and A4000/A5000. Imports check the resulting volume against the selected
sampler load target and block volumes that exceed its limits before writing.
This checks a clean full-volume load, not waveform RAM or merging into existing
sampler memory; see [Volume Load Capacity](docs/axklib/volume-capacity.md).
Floppy and package source pickers remember their last-used directories.

### Work With Files

- Create directories, rename entries, drag selected files and folders to move
  them within a partition, and review batch deletion
  on supported writable SFS, FAT16 and EX5 filesystems.
- Import files and directory trees through a picker or drag-and-drop; export
  selected files and folders, or drag copies out to the operating system.
- Drop `.ima` or `.img` files into a writable FAT16 destination, including EX5
  HD/MO images, and choose **File** to copy each image unchanged or **Contents**
  to select files and folders inside it. Review names and conflicts before import.
- Import a complete SU700 floppy into a new volume on an existing recognized
  SU700 hard disk. Divided floppy sets and merging into existing volumes are
  not supported.
- Inspect filesystem attributes and storage details. FAT destination names are
  uppercased automatically and checked against 8.3 filename limits.

Raw file imports copy payloads as stored. Filesystem edits do not repair
sampler-object relationships or convert files between device formats. Use
Device-mode object operations when those relationships need to be maintained.

### Export And Author Media

- Export selected Samples as mono or interleaved stereo WAV files, selected Wave
  Data as mono WAV files, individual object packages, and dependency-complete
  volume packages.
- Export SFZ instruments with supported forward playback and loop modes;
  reverse and bidirectional playback are not supported, and release-tail
  transitions cannot be preserved. See [SFZ export](docs/axklib/sfz.md).
- Batch-export every volume in a partition as packages or as per-volume floppy
  sets.
- Create formatted A-series HDS images up to 8 GiB with valid partition
  combinations, including 128 and 256 MiB sizes for smaller test images, or
  blank 1.44 MB floppy images. See [image authoring](docs/axklib/write.md)
  for size and compatibility limits.
- Export A-series multi-floppy sets and ISO9660 CD-ROM images. EX5 and SU700
  formatted-image creation is not offered.
- Insert, delete, rename, and repair supported image content through planned,
  transactional alterations with rollback protection.

### Preferences And Diagnostics

- Save a **Preferred A-Series generation** in Preferences to preselect audio
  import and sampler load targets; each operation can override the suggestion.
- Open **Logs...** in the status bar to view Application and bundled local-server
  logs, filter by source, minimum level and search, copy text selections, or
  save the filtered view or all retained logs. Remote-server logs are not included.

See the [axkdeck development guide](apps/axkdeck/README.md) when building the
desktop application from source.

## Local And Remote Use

`axklib` handles media formats and image operations, `axklib-server` exposes
those capabilities through an API, and axkdeck provides the desktop interface.
Normal desktop use includes a bundled local server. This separation also allows
axkdeck to work with images on a separately configured remote host, such as a
Raspberry Pi used for PiSCSI, without first copying whole images to the desktop.

This is remote image access, not PiSCSI hardware control or live editing of disks
mounted by a sampler. See [server configuration](docs/axklib/server.md#configuration)
and the [Raspberry Pi deployment profile](docs/axklib/server.md#low-concurrency-deployment-profile)
for authentication, network security and resource requirements.

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

The CLI provides image inspection, validation, extraction, package transfer,
creation, and alteration for scripts and batch workflows. For example:

```bash
axklib info HD00_512_example.hds
axklib validate -o validation HD00_512_example.hds
axklib extract wav file -o wav HD00_512_example.hds
```

Alteration manifests also support parameter updates for supported Programs,
Samples, Sample Banks, and Wave Data, replacement of Program assignments, and
retargeting a Sample's Wave Data references. These scripted operations are
distinct from axkdeck's desktop editing controls; see
[existing image alteration](docs/axklib/alteration.md) for their supported
models, layouts and validation requirements.

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
