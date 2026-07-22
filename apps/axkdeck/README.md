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

After installing the system packages, verify the complete toolchain from the
`axkdeck` directory:

```bash
rustc --version
node --version
corepack enable
corepack pnpm install --frozen-lockfile
corepack pnpm tauri info
```

All entries in the `Environment` section of `tauri info` should show as
available before running `corepack pnpm tauri build`.

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
AXKDECK_LOG_LEVEL=debug corepack pnpm tauri dev
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

## Develop

```bash
corepack pnpm install
corepack pnpm dev
```

The Vite frontend is available at `http://localhost:5173`.

To launch the desktop shell with the existing native release server:

```bash
corepack pnpm desktop:dev
```

Use `corepack pnpm desktop:dev:fresh` to incrementally build the native server
target before launching the desktop shell.

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

### Create an empty hard-disk image

Open the disk-image browser, enter the writable workspace directory that should
own the new file, and select **New hard disk image**. The creation dialog offers
only server-published capacity and partition combinations. It creates the image
in that exact directory and opens the completed file when the background job
finishes.

The floppy-scale and CD-R-scale capacities in this dialog create small HDS
images. They are convenient package/import targets for later transfer
workflows; they are not empty floppy or ISO containers. Direct empty FAT12 and
ISO9660 creation is intentionally not offered because those media need Yamaha
catalog and object content to be useful.

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
corepack pnpm test
corepack pnpm check
corepack pnpm build
cargo fmt --manifest-path src-tauri/Cargo.toml -- --check
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
corepack pnpm tauri build
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
view, or drag WAV, FLAC, or AIFF files onto the active workspace or a writable
volume in the Volumes sidebar. Axkdeck stages at most three files concurrently
and shows the source format, sampler conversion, generated Sample/Wave Data
names, and root key before changing the image. One accepted batch is applied as
one atomic image alteration. Mono input creates one standalone Sample (SBNK)
and one Wave Data (SMPL) object; stereo input creates one Sample linked to left
and right Wave Data objects.

Wave Data auditioning uses the server's bounded HTTP range resource to assemble
one complete WAV before playback. The browser decodes that WAV off the audio
render path, keeps a bounded decoded-audio cache, and schedules a native Web
Audio buffer source. Playback never depends on network delivery from the
real-time audio callback. Forward loops use native Web Audio loop points;
reverse modes use one reversed decoded buffer.

The versioned operation and DTO boundary is documented in
[`docs/service-boundary.md`](docs/service-boundary.md). The Crow OpenAPI contract
is authoritative for all axkdeck domain behavior.
