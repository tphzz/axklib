# axkdeck

`axkdeck` is a desktop-first Yamaha A-series disk and sample workspace. Desktop
builds package `axklib-server` as a loopback sidecar and use its versioned
HTTP/WebSocket API. The Tauri shell owns sidecar lifecycle and protected remote
server settings but contains no duplicate axklib domain implementation.
Production builds contain no scripting runtime or demo-data fallback.

## Stack

- Svelte 5 with runes and TypeScript
- Vite 8 (Rolldown) for the standalone web frontend
- Tailwind CSS 4 through its first-party Vite plugin
- Tauri 2 for cross-platform desktop distribution
- pnpm with Corepack for reproducible JavaScript tooling
- C++23 axklib application service and Crow-based `axklib-server`
- HTTP/WebSocket transport for local sidecar and remote Raspberry Pi profiles

## Prerequisites

- Node.js 22.12 or newer
- Corepack (`corepack enable`)
- Rust stable
- CMake 3.28 or newer and Ninja
- The [Tauri platform prerequisites](https://v2.tauri.app/start/prerequisites/)
- A configured axklib release build in the monorepo root

### Native dependencies

Axkdeck packages the `axklib-server` produced by the monorepo native release
build. From the repository root on Linux or macOS:

```bash
git submodule update --init --recursive
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset release
cmake --build --preset release --target axklib_server
```

On Windows PowerShell, run the equivalent commands from the repository root:

```powershell
git submodule update --init --recursive
.\external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake --preset release
cmake --build --preset release --target axklib_server
```

Axkdeck is a Cargo/Tauri project and intentionally is not part of the CMake
project. Run `corepack pnpm desktop:build` from `apps/axkdeck`. Its Rust build
script stages the existing native release server and never configures or builds
the C++ project. Set `AXKLIB_SERVER_BINARY` only when using a nonstandard native
build directory.

The same Git tag versions axklib and axkdeck. `desktop:dev` and
`desktop:build` read `version_metadata.json` and `package_basename.txt` from the
native build, inject that identity into Tauri, and embed it in the Rust binary.
The tracked npm and Cargo versions are `0.0.0` placeholders and are not release
version sources. A locally launched desktop refuses a bundled sidecar whose
semantic version or source identity differs, so rebuild the native server after
changing commits.

### Linux

Tauri uses the system WebKitGTK webview on Linux and therefore requires native
development packages in addition to Node.js and Rust. On Debian 12, Ubuntu
22.04, or newer, install the packages recommended by Tauri:

```bash
sudo apt update
sudo apt install \
  libwebkit2gtk-4.1-dev \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  curl \
  wget \
  file \
  libxdo-dev \
  libssl-dev \
  libayatana-appindicator3-dev \
  librsvg2-dev
```

For Arch, Fedora, openSUSE, Alpine, NixOS, and other distributions, use the
distribution-specific package list in the official
[Tauri Linux prerequisites](https://v2.tauri.app/start/prerequisites/#linux).

After installing the system packages, verify the complete toolchain from
`apps/axkdeck`:

```bash
rustc --version
node --version
corepack enable
corepack pnpm install --frozen-lockfile
corepack pnpm tauri info
```

All entries in the `Environment` section of `tauri info` should show as
available before running `corepack pnpm desktop:build`.

#### Linux graphics compatibility

axkdeck disables WebKitGTK's DMABUF renderer by default on Linux. This avoids a
known WebKitGTK graphics-driver incompatibility that can leave a Tauri window
blank or cause it to flicker. The application does not disable accelerated
compositing generally; only the problematic DMABUF rendering path is bypassed.

Users with a known-good Linux graphics stack can opt back into the faster
DMABUF path when launching axkdeck:

```bash
AXKDECK_ENABLE_DMABUF=1 axkdeck
```

See [Tauri's Linux graphics guidance](https://v2.tauri.app/develop/debug/linux-graphics/)
for the upstream symptoms and renderer tradeoffs.

#### Logging

`AXKDECK_LOG_LEVEL` controls desktop logging at runtime. Supported values are
`trace`, `debug`, `info`, `warn`, `error`, and `off`; the default is `info`.
Detailed audio playback telemetry is generated only at `debug` or `trace`,
including preparation, transfer, decoding, cache, and scheduling timings. For
example:

```bash
AXKDECK_LOG_LEVEL=debug corepack pnpm desktop:dev
```

### macOS

Desktop builds require macOS 10.15 or newer and Apple's native build tools.
Install the Xcode Command Line Tools:

```bash
xcode-select --install
```

Full Xcode is only required when targeting iOS. If it is installed instead,
launch it once to complete its setup and accept the license. Then verify the
desktop toolchain:

```bash
xcode-select -p
rustc --version
node --version
corepack enable
corepack pnpm install --frozen-lockfile
corepack pnpm tauri info
```

See the official
[Tauri macOS prerequisites](https://v2.tauri.app/start/prerequisites/#macos)
for the Xcode installation options.

### Windows

Windows builds require the Microsoft C++ Build Tools and Microsoft Edge
WebView2:

1. Install the
   [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
   and select the **Desktop development with C++** workload.
2. Ensure the
   [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/#download-section)
   is installed. It is already included with Windows 10 version 1803 and newer.
3. Install Rust with the MSVC host toolchain and use Node.js LTS.

Open a new PowerShell terminal and verify the toolchain:

```powershell
rustup default stable-msvc
rustc --version
node --version
corepack enable
corepack pnpm install --frozen-lockfile
corepack pnpm tauri info
```

See the official
[Tauri Windows prerequisites](https://v2.tauri.app/start/prerequisites/#windows)
for installer details and troubleshooting.

Tauri desktop packages are native to the build host. The native CI matrix builds
the C++ targets once per platform and then reuses the resulting server for the
matching axkdeck build. Release packaging produces a universal macOS DMG,
separate x64 and ARM64 NSIS installers, and matching DEB and RPM packages.
These installers are attached directly to the GitHub draft release alongside,
but separately from, the native axklib SDK and CLI archives. Each installer
contains the matching tested `axklib-server` sidecar.

## Develop

Run the following commands from `apps/axkdeck`. The native release server must
already exist at `build/native/release/apps/server/axklib-server` relative to
the monorepo root, as described under **Native dependencies** above.

```bash
corepack pnpm install --frozen-lockfile
corepack pnpm dev
```

The Vite frontend is available at `http://localhost:5173`.

To launch the desktop shell with the existing native release server:

```bash
corepack pnpm desktop:dev
```

Use `corepack pnpm desktop:dev:fresh` to incrementally build the native server
target before launching the desktop shell.

### Interface scale

Desktop builds adjust the webview scale before mounting the interface. Auto
mode uses the active monitor's physical resolution together with its operating
system scale factor, so a 4K display at 100% receives a larger interface while
a display that is already scaled by the operating system is not enlarged
twice. The scale is recalculated when the window moves to another monitor or
the monitor scale changes.

Use the sliders menu beside the panel layout controls to select Auto, 100%,
125%, or 150%. The selected mode is stored locally and restored on the next
launch. Manual modes remain fixed when the window moves between displays.

### Local workspaces

The sidecar starts without inventing a workspace. On first launch, axkdeck asks
for a directory through the native operating-system folder dialog. Give it a
display name and choose whether image creation and changes are allowed. Use the
workspace button in the application header to add or remove directories later.

The server persists this list in the current user's platform configuration
directory. Missing or inaccessible directories remain visible with their
status instead of silently disappearing. Remote axkdeck connections use a
directory-only browser for the remote host; after selection, ordinary image
and output access remains confined to the configured workspaces.

The image browser opens supported image and standalone object files normally.
It also recognizes flat leaf directories containing externally extracted
Yamaha objects and labels them **Sampler object folder**. Select that row to
open it directly. The resulting `AXK_OBJECT_DIRECTORY` session is read-only and
supports browsing, preview, audition, and package export. Parent collection
directories remain navigation folders unless they are themselves recognized as
one bounded multi-disk object set.

Opening one sampler object folder does not scan nearby folders. If playback or
package export needs Wave Data continued on another sampler disk, axkdeck asks
for companion disk folders at that point. Select only the folders belonging to
the same disk set, or explicitly use the nearby-folder search. The association
lasts only for the open image session; axkdeck does not copy, merge, or modify
the source folders.

### Create an empty HD or floppy image

Open the disk-image browser, enter the writable workspace directory that should
own the new file, and select **Create image**. In **Create HD/Floppy image**,
choose **HD** or **Floppy**. HD creation offers only server-published capacity
and partition combinations. Floppy creation asks only for a filename and always
writes the fixed 1.44 MB Yamaha A-series FAT12 full-format profile. The
background job publishes the image in that exact directory and opens the
completed file.

The floppy option is a blank formatted disk with the Yamaha catalog and marker
files, not a populated authored-volume image. Empty ISO9660 creation is not
offered; use the ISO authoring or conversion workflows when object content is
available.

### Import and export portable packages

Right-click a volume in the image sidebar to import or export an axklib
portable package. Import accepts a package from any configured storage
location. The desktop application can also choose a local package, upload it
in bounded chunks, and remove that temporary upload when the dialog closes.
Axkdeck verifies the package, shows its Program, Sample Bank, Sample, and Wave
Data graph, and presents the exact insertion, reuse, allocation, warning, and
naming-conflict plan before enabling **Import package**. Conflict renames are
replanned before application.

Right-click a writable SFS partition and choose **Import packages…** to select
up to 256 `.axkvol` files at once. Axkdeck uses each package's volume placement
hint to propose one new destination volume, adds numeric suffixes where names
would collide, and previews the Program, Sample Bank, Sample, Wave Data, and
Sequence counts for every volume. Destination names remain editable and must be
checked again after a change. Removing a row excludes that entire package. The
confirmed set is applied as one journaled mutation: either every previewed
volume is created or none of them are.

Session import is an atomic, journaled change to the currently open writable
SFS image. The plan is bound to the image revision and retained package
identity; an outdated plan cannot alter the image. After a successful job,
axkdeck refreshes the same image session and restores the first destination
volume.

Volume export creates one `.axkvol` containing the complete admitted object
dependency graph. Programs, Sample Banks, Samples, and Wave Data can also be
selected into one image-wide export basket. The selection remains available
while changing tabs, filters, or volumes. Plain click starts a new selection;
Ctrl/Cmd-click toggles one object, Shift-click replaces the current list's
range, Ctrl/Cmd+Shift-click adds a range, and Ctrl/Cmd+A adds every visible row
in the focused list. The header shows the total and exports or clears the
basket. Right-clicking a selected object exports the complete basket;
right-clicking an unselected object starts a new one-object selection. Up to
1,024 roots can be selected atomically, and shared dependencies are included
once. Homogeneous selections keep their typed package extension; only mixed
root kinds use `.axkpkg`. An export can publish directly to a configured
writable storage location. The desktop application can instead choose a local
destination; the server then retains an owner-scoped package briefly while the
native shell streams it to the selected file and deletes the retained resource
afterward. Remote browser clients use configured server storage because they
cannot write an arbitrary client filesystem path.

The same image-wide basket supports deletion in writable SFS sessions.
Right-click **Delete** or use the header action to preview the complete batch.
The confirmation dialog separates eligible selections from objects blocked by
incoming or ambiguous references, and offers newly unreferenced dependencies as
explicit opt-in cleanup. Blocked objects remain untouched; the eligible subset
is published atomically after the image revision and impact are rechecked.

The Wave Data view adds a broom action for volume-scoped storage cleanup.
It lists only Wave Data that the server can confirm is unreferenced, selects
the candidates by default, and lets the user review or deselect each object.
The dialog reports recoverable bytes and clusters and uses an explicit
**Delete N Wave Data objects** confirmation. Axkdeck repeats orphan discovery
and the normal deletion inspection immediately before starting the job; a
changed or newly referenced candidate returns to review instead of being
deleted. Discovery is capped at 1,024 candidates per pass.

### Diagnostics

Development builds open the web developer tools with `F12`. This shortcut is
not enabled in release builds. Desktop/frontend messages and sidecar output are
stored separately as `axkdeck.log` and `axklib-server.log`. Each log rotates at
5 MiB and retains three files in the platform application log directory:

- `%LOCALAPPDATA%\app.axkdeck.desktop\logs` on Windows;
- `${XDG_DATA_HOME:-~/.local/share}/app.axkdeck.desktop/logs` on Linux; and
- `~/Library/Logs/app.axkdeck.desktop` on macOS.

Development runs also mirror `axklib-server` stdout and stderr to the terminal.
Workspace setup failures remain visible in the Workspaces dialog and include
the server request ID when one is available.

## Verify and build

```bash
corepack pnpm format:check
corepack pnpm contract:check
corepack pnpm version:check
corepack pnpm version:test
corepack pnpm test
corepack pnpm check
corepack pnpm build
cargo fmt --manifest-path src-tauri/Cargo.toml -- --check
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
corepack pnpm desktop:build
```

TypeScript and Svelte source is formatted with the project-local
`.prettierrc.json`: four-space indentation, spaces only, and a 120-column
limit. Run `corepack pnpm format` after each frontend implementation pass, then
use `corepack pnpm format:check` to verify the result.

The web build is written to `dist/`. Tauri installers and application bundles
are written below `src-tauri/target/release/bundle/`.

## Architecture

The UI uses one typed transport interface. Desktop mode launches the packaged
`axklib-server` sidecar on a kernel-selected loopback port, consumes and deletes
its owner-only connection file, and uses the same HTTP/WebSocket transport as a
configured remote server. Remote credentials are stored by the operating-system
credential manager. Images and outputs remain inside persisted server
workspaces; client uploads are limited to audio, manifests, and portable packages.
The shell also passes its process ID to the sidecar; the sidecar exits promptly
if an abnormal shell termination prevents the normal authenticated shutdown.

UI components do not contain transport-specific domain models. Persistent files
are selected inside server sandbox roots; browser-local audio, manifests, and
portable packages enter through bounded uploads. Explicit save actions stream
server files or bounded directory archives without moving image ownership into
the browser.

To import audio, open a writable volume and use **Import audio** in the Samples
view. Choose one or more WAV, FLAC, or AIFF files directly from a configured
storage location, or choose files from **This computer** and upload them. Audio
files can also be dragged onto the active workspace or a writable volume in the
Volumes sidebar. All three entry points use the same inspection,
collision-resistant Sample/Wave Data naming, and review flow. Axkdeck stages at
most three files concurrently and shows determinate batch-inspection progress.
Generated names are assigned in source order only after the complete batch has
been inspected, so partially prepared rows are not presented as validation
errors. The review cards show the source format, sampler conversion, generated
names, sampler settings, and non-fatal metadata adjustments before changing the
image. One accepted batch is applied as one atomic image alteration. Mono input
creates one standalone Sample (SBNK) and one Wave Data (SMPL) object; stereo
input creates one Sample linked to left and right Wave Data objects.

Each prepared card has a play/stop action beside its source filename. This is a
client-side source preview that applies the card's current one-shot or forward
loop fields; it does not run the server's final conversion pipeline. Very short
one-shots use the same bounded 500 ms repeat schedule as Wave Data auditioning,
so single-cycle files remain audible. Preview decode failures are row-local and
do not invalidate an otherwise importable file.

Wave Data auditioning uses the server's bounded HTTP range resource to assemble
one complete WAV before playback. The browser decodes that WAV off the audio
render path, keeps a bounded decoded-audio cache, and schedules a native Web
Audio buffer source. Playback never depends on network delivery from the
real-time audio callback. Forward loops use native Web Audio loop points;
reverse modes use one reversed decoded buffer. Interactive playback repeats a
complete forward one-shot at or below 50 ms for 500 ms with bounded fades so
single-cycle and similarly short audio remains audible. This preview-only
schedule does not change the Sample or Wave Data loop metadata.

The versioned operation and DTO boundary is documented in
[`docs/service-boundary.md`](docs/service-boundary.md). The Crow OpenAPI contract
is authoritative for all axkdeck domain behavior.
