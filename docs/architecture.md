# Architecture

The native implementation separates storage, sampler semantics, and host
integration.

## Desktop Workspace Modules

Axkdeck separates the shared workspace shell from backend orchestration and
device presentation. `src/App.svelte` selects a bundled backend application from
`features/backends/registry.ts`; the existing axklib transport, A-series catalog,
audition, editing and dialog workflows live behind `features/backends/axklib/`.
An alternative backend application does not implement `ImageTransport` or use
A-series object types merely to enter the shell.

`WorkspacePresentation` supplies navigation, central content, optional lower
tools and an optional inspector. It can also supply object tabs, selection
actions and playback controls. All pane geometry, visibility toggles, interface
scaling and the Device/Files switch belong to `WorkspaceShell`. The common image
action area is supplied separately from presentation-specific navigation. The
axklib backend supplies the current A-series Device presentation; adding another
device presentation must not introduce device switches into the shared shell.

Object editing is organized independently of backend selection. Shared draft,
history, save/recovery and exit-confirmation behavior lives in
`features/object-editor/`; device/object pages and parameter semantics live in
`features/devices/a-series/sample/`. The editor registry selects the explicit
server-provided `a4000-a5000/sample` profile. It does not infer a device from a
filename or add device-specific branches to Files mode. Future device profiles
provide their own field definitions, validation, write mapping and audition.

Drafts are keyed by session and object identity. A revision change revalidates
the original payload digest and placement before a draft can save; a changed
payload remains a conflict until explicitly discarded. Saving sends only changed
parameters with revision and payload preconditions, through the existing atomic
mutation job. Playback-window and loop changes share that transaction. Browsing
another Sample, volume or mode does not write or discard drafts. Source object
bytes are read on demand for the bounded editor snapshot rather than retained in
every catalog entry. Desktop window-close and application-quit guards cover both
dirty drafts and unresolved writes.

The Sample editor keeps main tabs beside identity and Save actions, with one
active subpage and a persistent draft-audition strip. Waveform boundaries are
presented as start/end positions and mapped to the native start/length contract;
numeric entry and drag gestures share the same validation and undo history.
Level and filter scaling use native MIDI-note coordinates, with a keyboard and
precise breakpoint controls. Common numeric and choice controls live in the
object-editor layer, while parameter groupings remain device-specific.
Shared spacing tokens keep panel padding, section gaps and table cells compact
without shrinking text or input targets. Form-only pages use left-aligned,
bounded columns; intermediate widths keep Output routing below Mix and Pitch
bend below Sample portamento. Entirely unavailable groups use compact columns
instead of reserving slider-width space. Missing columns wrap rather than stretch
apart. The identity header reserves dirty-marker space so tabs do not move on edit.
Collection identities also reserve a compact dirty-marker slot; unsaved edits
cannot change name/metadata height, stereo-indicator placement or name truncation.

Graphical pages share a resizable graph/controls layout, plot frame, keyboard
axis and compact parameter groups. Wide panes start with equal-width columns;
below 900 CSS pixels the graph stays above switchable control groups. Controls
scroll independently, and layout choices do not dirty the object. The shell
accepts a preferred lower-pane height from its presentation; Sample editing
requests 360 CSS pixels or one third of the workspace, subject to available
height and manual resizing. Waveform editing remains full-width. Workspace and
graph splitters use one shared component with a single line inside an eight-pixel
pointer target, keyboard resizing and a reset action.

Responsive editor thresholds use shared `ResizeObserver` measurements of layout
width, not zoomed bounding rectangles or CSS container queries. This keeps
stacked control-group navigation aligned with layout under native WebKitGTK
page zoom. Choice controls use measured label widths before showing segments;
otherwise they use a bounded menu. The audition strip reserves stable button
and status geometry across playback transitions.
Sample settings keeps source duration alongside sample-rate metadata, rather than
in a separate footer. It describes the underlying source, not the trimmed or looped range.
Editor popups observe their rendered size and keep the edge nearest the field
anchored as filtering changes the list height. Opening direction stays fixed until
dismissal; content scrolls within the available space on that side.

Envelope stage models and parameter bindings belong to the device adapter.
Amplitude, filter and pitch share a full-width stage presentation while
retaining their distinct level ranges, editable endpoints and Hold behavior.
Envelope spacing responds to native rates; horizontal handles edit rates and
vertical handles edit levels in one undoable gesture. Release is edited at its
endpoint, not an artificial midpoint on the slope.
The Amplitude release endpoint stays at zero; Filter/Pitch endpoints edit both
release rate and level. Spacing is relative,
without calibrated transition times. Maximum-rate transitions do not receive a
visible minimum-width ramp; AEG release 127 is immediate as documented by Yamaha.
Offscreen handles are hidden inside the fixed viewport, with Fit/zoom restoring
access. Rate dragging may extend beyond the viewport without expanding its drawing
surface or changing the bounded level domain. LFO plots show relative speed, delay and
buildup, with independent pitch, amplitude and cutoff traces and an illustrative
Sample & Hold pattern. Sample & Hold speed belongs to the Program. Delay shifts
the clean oscillator trace; a separate guide shows relative buildup rather than
distorting the first cycle. Analytical corners retain vertical discontinuities.

Graph titles, readouts and tools share one fixed-height row outside the curves. Shared drag
handling freezes the coordinate frame and grab offset for a complete pointer
gesture, coalesces movement per animation frame, and flushes the release position.
Draft patches publish related values atomically. Canvas buffers and theme colors
are refreshed on size or theme changes, not on every parameter edit.
Envelope viewports persist per draft and envelope page until explicit
zoom or Fit to width; editing, releasing a handle and undo do not auto-fit.

Filter and Sample EQ have separate graphical subpages. Generic plotting and
handle components accept neutral traces and coordinates; A-series adapters own
the parameter bindings and response models. Filter curves are schematic and
use native cutoff/Q values, not calibrated frequency units. The EQ response
uses the pre-quantization parameter response for its solid editing curve. An
optional dashed overlay shows stored Q13 coefficients until an EQ parameter
changes, then regenerated draft Q13 coefficients. Low-frequency coefficient
rounding can substantially displace the plotted peak, so it is not used to position
the editing curve. Neither curve is a measured hardware response. The native
writer's coefficient equations and maintained reference vectors are unchanged.
These plots do not extend
audio preview into EQ, filter, envelope or LFO synthesis.
EQ uses one frequency/gain handle. Wheel or Alt-drag edits Peak/Dip width;
normal wheel and Alt+Arrow steps are 0.5, or 0.1 with Shift. Alt-drag uses twice
the original width sensitivity; Shift-drag reduces pointer sensitivity fourfold.
Frequency snapping has hysteresis around
native discrete boundaries, and each drag or wheel burst is one undo step.
Shelves retain fixed width. These interaction changes do not alter coefficient math.

Velocity range uses a compact vertical soft-to-hard MIDI velocity band beside
paired numeric boundaries and crossfade controls. It is not a keyboard range
or a calibrated loudness meter. MIDI/CTRL choices use shared searchable,
keyboard-accessible popups. Clearing a search preserves the assigned parameter;
only choosing an option changes the draft. Popup placement, dismissal, numeric
suffix alignment and slider focus treatment are shared across editor controls.
The Control subpage presents all six assignments as numbered rows with
Controller, Function, Type and Range columns, retaining distinct accessible
field names. Missing or unsupported stored values display Unavailable instead
of disabled placeholder inputs. Read-only per-field API metadata distinguishes
settings absent from the stored layout from unrecognized stored values; help is
available on hover and keyboard focus. Stereo-blocked fields retain their values and explain the
restriction; neither case grants new write capabilities.

Position units, zoom, snapping, monitor lead-in, beat-count selection, preview
note and audition volume are local view settings. Only an explicit tempo
calculation or parameter edit changes the draft. Source-rate PCM is loaded on
demand for zoom, zero-cross snapping and audition, with a 128 MiB working limit
and temporary-resource cleanup. Changing editor identity or its source snapshot
releases cached PCM and stops its preview. Loop Remix, destructive PCM operations
and complete synthesis preview remain outside the implemented editor.

The reusable Files components depend only on `FilesystemAccess` and neutral
entry/page contracts in `lib/filesystem.ts`. A backend adapter translates its
wire representation into that contract. It provides stored entry identities,
parents, roots, names, sizes, paging and full-root search; it does not project
sampler relationships into synthetic directories. The sidebar selects roots or
partitions, while the central tree owns directory expansion. Unknown files stay
visible without a device interpretation. Files supports browsing, inspection,
directory creation and confirmed file/recursive directory deletion on writable
SFS roots. Raw file/directory exports are also available for readable roots,
including read-only images. Import review and FAT mutation controls use the
same per-root capability and transaction contracts.

Files selection is scoped to the active root and rendered entries. Modifier
clicks and keyboard ranges support batch deletion; context menus retain an
existing selection. Focus-only navigation does not alter the batch. Changed
searches clear selection, and collapsing a directory replaces selected hidden
descendants with their visible parent. Refresh restores surviving unique paths,
not reused entry IDs. Deletion review includes the complete selection; execution
removes redundant descendant targets and submits one atomic job.

Creation and import destinations use actual selection, not the retained
keyboard cursor: a selected directory, a selected file's parent, or the active
root after deselection. Background clicks clear selection without discarding
expansion or scroll. Dialogs capture their destination before editing starts.
Both cross-mode inspector actions use the fixed `InspectorModeFooter`, labeled
**To Files** and **To Device**, outside the scrolling properties.

The axklib-specific `AxklibFilesView` supplies a SU700 floppy import adapter.
Generic Files components only know its command and drop-interception interface.
The backend advertises `supportedImports: ["SU700_FLOPPY"]` for writable SFS
roots containing a recognized SU700 volume. Empty or unrecognized roots do not
advertise this operation. Other backends can supply their own import adapters.

SU700 import accepts one complete, flat FAT12 image and creates a new named
volume with `SONGCONT.DAT`, `SUSQ` songs and `SUSP` samples. Stored eight-byte
basenames, including internal padding before the extension, and all payload
bytes are preserved. Extra files are included by default and can be deselected.
The shared destination chooser is new-volume-only; merge, overwrite, disk-set
reconstruction and SU700 object editing are not supported. Inspection validates
control references, complete payload boundaries, names and destination capacity.
Execution rechecks source identity and image revision, then uses one atomic
Files transaction with rollback. This is host-validated import support, not a
claim of hardware playback validation.

The separate `FilesystemMutationDriver` binds execution, observation,
cancellation and image refresh. The axklib adapter uses the shared job controller
and invalidates audition before mutation. Review captures the session, entry
identities and revision. A committed write followed by failed refresh can only
retry refresh; interrupted job observation checks the same job rather than
resubmitting its edits. Per-root capabilities disable unsupported controls.

`FilesystemExportActions` supplies the session-bound export job driver and host
destination adapter. Partition menus and the Files toolbar share one review
workflow with a frozen selection/revision, exact output paths and omission
notices. Managed-local batches use one native folder picker; remote sessions
use the existing destination chooser. The selected route survives failures.
Uncertain execution checks the existing job rather than resubmitting. Failed
local download publication retains its archive for an explicit destination
retry; successful publication or closing the review releases that archive.
Export does not mutate the image, refresh selection or repair object links.

Device and Files are independent capabilities. Recognized A-series objects
enable Device; empty SFS images also allow A-series authoring. Files-only images
open in Files with Device unavailable. Archives and standalone object sources
can have Device without a filesystem view. No Akai or E-mu backend is bundled.

The Files controller retains per-root selection, expansion, search and scroll
state while switching modes. Revision changes discard cached entries and
revalidate the active root's identities. Cross-view navigation requires a unique
physical entry/object mapping, opens ancestors, loads the required pages and
reveals the row once. It never guesses by display name. Split floppy sets retain
their separate physical roots without claiming an ambiguous combined-object
mapping. Loading and revision failures remain visible instead of falling back
to a projected object tree.

## Public Documentation Boundary

Public documentation describes supported contracts, limits, validation rules,
and concise hardware compatibility status. Small retained test fixtures may
document their identity, hash, topology, and active test purpose. Runtime
diagnostics describe the input contract and corrective action.

Format pages specify stored bytes, encoding, relationships and modification
constraints independently of a particular decoder. Unspecified meanings and
preservation requirements remain explicit. API symbols, report field names,
application workflows and generated-output conventions have separate contract
pages. Only installed SDK interfaces are presented as public C++ APIs.

```mermaid
flowchart TD
    accTitle: axklib architecture dependency flow
    accDescr: Random access input flows through media readers, Yamaha enrichment, object decoders, and the catalog. The catalog provides audio export, writing, the shared SDK facade, and the CLI adapter. Native consumers use the SDK.
    IO[Random access I/O] --> Media[SFS, FAT12, ISO9660, A3K, and object-directory readers]
    Media --> Yamaha[Yamaha media enrichment]
    Yamaha --> Objects[Object decoders]
    Objects --> Catalog[Catalog and relationships]
    Catalog --> Audio[Exact audio and SFZ]
    Catalog --> Writer[Fresh writer and transactions]
    Catalog --> SDK[C++17 PIMPL facade]
    Catalog --> CLI[CLI11 adapter]
    SDK --> Host[Native SDK consumers]
```

The private C++23 engine owns format behavior and typed errors. The shared SDK
facade owns PIMPL sessions, results, pagination, cancellation, and progress. The
CLI adapter owns argument parsing, exit codes, output layout, and report
serialization. The CLI links the private engine statically; SDK consumers load
the shared library.

The CLI follows a one-way dependency path:

`platform entry -> CLI11 registration -> typed request -> command family -> axklib service`

The source modules reflect that boundary:

- `apps/cli/main.cpp` and `apps/cli/command_line.*` convert platform arguments to checked
  UTF-8 and contain process-level failures.
- `apps/cli/app.*` registers the root command and dispatches typed requests.
- `apps/cli/commands/` owns independent analysis, extraction, report, package,
  and writer/transaction command families.
- `apps/cli/schema/` owns versioned machine-output data structures and their private
  JSON serialization.
- `apps/application/src/content_id.*` owns pooled-export identifiers and
  collision handling as an application-private extraction helper.

Command modules orchestrate public library services; they do not contain disk
layout, object decoding, allocation, or audio-conversion rules. Core targets do
not include CLI11 or CLI headers.

The media source modules preserve a separate responsibility boundary:

- `media_fat12.cpp` owns shared FAT directory and file reads; `media_ex5.cpp`
  supplies the separate read-only EX5 disk geometry and content projection.
- `media_iso9660.cpp` owns the supported primary ISO9660 container profile.
- `media_a3k_archive.cpp` owns the bounded read-only A3K archive profile.
- `media_build.cpp` inventories metadata before loading a selected dependency
  closure, enforces public aggregate build limits, and validates written
  payloads through bounded range readers.
- `media_write_fat12.cpp` adapts pinned FatFs to an in-memory 1.44 MB image.
- `media_write_iso9660.cpp` owns the deterministic narrow ISO9660 layout writer
  and writes projected sectors directly to the reserved temporary file without
  retaining a second output-sized image buffer.
- `media_yamaha.cpp` owns Yamaha object recognition, CD menu labels, catalog
  placement, flat AXK object directories, and structured paths.
- `media.cpp` owns common media dispatch and `MediaContainer` orchestration.

These remain source modules in one core target. They are not separately linked
SDK components.

Fresh-image and alteration operations use manifests and plans. Applying a plan
writes a temporary destination, validates the result, and then completes the
replacement. The output path must differ from the source image. Existing source
images therefore remain unchanged.

The private write/package implementation is split by invariant ownership:

- `alteration_manifest_json.cpp` owns canonical JSON serialization and typed
  parsing, while
  `alteration_manifest.cpp` validates a complete typed transaction before any
  image I/O. `alteration.cpp` owns stateful planning and mutation.
- `object_write_codec.cpp` and `sfs_write_codec.cpp` own bounded object-payload
  and directory-index encoding. `writer_image.cpp` owns HDS geometry,
  allocation, and publication.
- `package_manifest.cpp` owns canonical package JSON serialization and
  relocation bindings. `package_import_plan.cpp` owns immutable import-plan
  identity and verification; media-specific capacity planning remains in
  `package_import.cpp`.

These are private source boundaries, not installed APIs. Binary fields are
encoded through checked writers, and callers propagate an error instead of
indexing past a malformed or incorrectly sized buffer.
