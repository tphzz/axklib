# REST And WebSocket Server

`axklib-server` exposes the maintained axklib operations to axkdeck and other
authenticated clients. It uses upstream Crow for JSON REST routes and the
server-to-client job-event WebSocket. The server does not host the axkdeck web
application and does not accept commands over WebSocket.

The complete contract is available in the rendered
[OpenAPI reference](openapi.md), including a download of the canonical OpenAPI
3.1 JSON document used by native and frontend contract checks.

## Storage Model

### Floppy Session Identity

An image session's `floppySet` includes ordinary standalone Yamaha disks as
well as multi-disk sets. `ORDINARY` denotes the ordinary `A3000.SYM` marker;
it is distinct from `NONE`, `CONTINUATION`, `FINAL` and `INVALID`.
A member's `index` preserves a parsed two-digit catalog ordinal (1-99), or
is zero when its label contains no valid ordinal. Zero does not mean a missing
disk, and clients must not replace it with an invented disk number.
`nextRequiredIndex` reports the next ordinal indicated by a continuation
marker. These metadata ranges do not change the 32-member attachment limit.

### Files Inside An Image

`GET /api/v1/images/{imageId}/filesystem` lists the stored filesystem entries
inside an owned image session. It is separate from host workspace browsing and
the semantic `/content` tree. Supported containers are SFS, FAT12, standard and EX5 FAT16,
ISO9660 and physical members of a floppy set. Archives and standalone object
sources report `available: false`.

Every request supplies `expectedRevision`. Without a selector the response lists
roots. `parentId` lists immediate children; `rootId` with `query` searches names
throughout that root, including unloaded directories. `entryId`, `objectId` and
`contentScopeId` support identity-based lookup. Choose only one selector. Results
use `offset`, `limit` and `totalCount`; the normal server page limit applies.

Entries contain exact stored names, a root-relative path, parent/ancestor IDs,
kind, optional logical file size, direct child count and storage details.
`rawAttributes` retains the native SFS or FAT attribute value. Each item in
`attributes` contains a stable `code`, a display `label` and `value`, an optional
explanation in `description` (empty when unnecessary), and compact display text
in `summary`. An empty `summary` restricts an attribute to Storage details.
Unresolved meanings are omitted from this list; the raw value remains intact.
Clients must not derive permissions from display labels or translate SFS flags
into POSIX permissions. The `fat.read-only` code identifies the FAT read-only
flag independently of its label.

In axkdeck, Files rows and inspector Properties show SFS **File write flag:
Enabled/Disabled**, or the FAT **Read-only**, **Hidden**, **System** and
**Archive** flags. The SFS flag applies to ordinary data writes and extension;
it does not determine rename/deletion permission or overall image editability.
Directories have no write-permission summary: their write flag is temporarily
enabled during directory updates. The collapsed **Storage details** section
holds the raw value, record state, native record type where defined, allocation
policy, large allocation unit in clusters and bytes, filesystem reference count,
and **Temporary directory write flag**. Attribute labels provide contextual
help on hover, keyboard focus or click, explaining their values and limits
without adding explanation rows. Escape or an outside click dismisses the help.
Attributes shown in Properties are not repeated under Storage details.
The reference count is not a child count. None of
these presentation fields changes the server's write-admission checks.

`filesystemMetadata` identifies reserved SFS support entries. Those entries
remain visible and selectable even when their empty reserved index target is
absent from the payload-bearing inventory, without an unresolved-target warning.
`objectId` is present only for a unique mapping to a decoded object. Entry IDs
are opaque: clients must not derive record numbers or links from them. A changed
revision invalidates the cached index and clients must revalidate selections.
SFS traversal bounds cycles and depth, and all indexes enforce an entry-count
limit and an estimated metadata-memory budget. Unresolved directory targets are exposed with an issue and unknown size,
not interpreted as an empty supported object. This API does not authorize any
filesystem mutation or raw file export.

Every page includes `rootCapabilities`, keyed by the opaque root ID. The
`createDirectory`, `putFile` and `deleteEntry` flags report supported raw writes
for that root, independently of device-object editing. Currently these are
enabled for SFS, standard plain/primary-MBR FAT16 and EX5 HD/removable roots
that pass the writer's filesystem admission checks, with a writable file source
and transaction path reservations available. Other profiles remain read-only.
Unsafe FAT metadata also leaves the root read-only without hiding readable files.
`maximumNameBytes`, `namePattern` and `nameHint`
describe new entry names for the frontend; actual writes validate names again.
Capabilities are not permission grants or conflict inspections: a particular
entry may still be protected, and revisions and current write conditions are
rechecked at execution. Clients must submit the revision reviewed by the user,
not silently replace it with a newer heartbeat revision.

`POST /api/v1/image-filesystem-edits` submits an idempotent raw filesystem mutation job.
The request supplies `imageId`, `expectedRevision`,
`acknowledgeDeviceRelationships: true` and an ordered `edits` array:

- `CREATE_DIRECTORY`: `parentEntryId` and a `relativePath` component array.
- `PUT_FILE`: the same destination, `source` containing exactly one `fileRef`,
  `uploadRef` or retained `imageEntryRef`, a required `expectedSource` snapshot from input inspection,
  and optional `conflict` (`SKIP` by default or `REPLACE`).
- `DELETE`: `entryId` and explicit `recursive`. Nonempty directories require
  `recursive: true`; partition roots and structural metadata remain protected.

Entry IDs are resolved inside the session, not interpreted by clients. All
changes must target one partition. Jobs reserve host input files and retain
upload leases; the session owns the exclusive image lease during its journaled
transaction. Stale revisions, invalid paths and failed transactions do not
partially apply a batch. The result reports the new revision and any new completion
warnings. The acknowledged notice that raw filesystem changes do not repair sampler
relationships is not repeated as a completion warning. A clean Files import closes
after temporary-resource cleanup and workspace refresh; new warnings or recovery
errors remain visible. Axkdeck exposes
New directory and confirmed file/recursive directory batch deletion through this
job. A selected directory covers selected descendants; the GUI submits only
top-level selected targets after showing the complete selection for review.
Axkdeck also exposes batch Add files through the Files toolbar and the entry
context menu's Import submenu. A selected file uses its parent directory; with
no selection the active partition/root is the destination. The review captures
that destination and image revision before choosing sources. Partition and
entry Import submenus also expose Import from disk for recursive workspace
directory contents. All these controls use the active root's capabilities and
name constraints. Root capabilities include `namePolicy`: `PRESERVE` retains
case, while `FAT_8_3_UPPERCASE` uppercases ASCII letters in destination names.
FAT names must fit 8.3 limits; no aliases are synthesized from incompatible
source names. Source paths remain unchanged. Each editable name has a validation
indicator with a tooltip explaining rejected names or destination conflicts.

Dropping raw files or directories onto a Files directory row
targets that directory; empty tree space targets the active root. The pointer
target takes precedence over object selection. File rows, metadata entries,
read-only roots and busy reviews do not accept drops. A dropped folder retains
its own name and empty children, unlike the contents-only directory picker.
The shared review still requires confirmation before any image write.

Browser acquisition uses the [File and Directory Entries API](https://wicg.github.io/entries-api/).
Handles are captured synchronously and directory batches are read until exhausted.
The scan is bounded to 10,000 entries, 63 path components, 4,194,304 serialized
path characters and 8 GiB of aggregate payload. Each file fits a 32-bit size.
Read errors, duplicate/cyclic paths and cancellation reject the complete scan;
no partial scan is uploaded. Uploads arriving after dismissal are released.
Native desktop drop-in uses the same review and limits. The desktop reader
checks the drop-granted filesystem scope and stops directory enumeration at the
remaining entry budget. Links, reparse points and special files are rejected.
File contents are read lazily in bounded chunks, with size, available identity
and modification-time checks before and after reading; changed sources fail the
import rather than committing a partial tree. The single native drop listener
routes raw Files paths before Device-specific extension filtering. Pointer
targeting accounts for the platform's native coordinate units and interface zoom.
Desktop Files rows also support copy-only native drag-out for the selected
files/directories. Ordinary clicks do not export. A deliberate drag captures
the current revision and selection, uses the existing download export job and
stages its exact files before handing them to the OS. Releasing before
preparation finishes cancels the handoff. Changed images, protected selections
and exports needing filename/omission review do not silently proceed; the latter
use the ordinary Export review instead. No image entries are moved or deleted.

The desktop cache uses opaque, single-use preparation tickets, owner-only
directories and cross-process leases. It reserves both archive and extraction
bytes against an 8 GiB logical-byte quota and a 16-export limit; entry counts
are bounded independently. Unused tickets expire after ten minutes. Handed-off
files remain available for 24 hours, including across application restarts,
because the receiving application may read paths after the drop completes.
Cleanup runs while the app is open and before new reservations; live downloads
and native drags retain leases. A full cache rejects new drag exports rather
than removing recently handed-off files. Export to disk remains available.
Linux uses GTK URI-list copy negotiation with escaped paths and a data provider
retained through drag-end. Windows/macOS use the native `drag` adapter in copy
mode. Linux acceptance covers consecutive directory, single-file and
multiselection drags, early release and Escape cancellation at 100% and 150%
interface scale, including delayed receiver reads. The GTK adapter completes
the initiating widget's pointer sequence after native handoff, so the next drag
does not consume an extra click. Windows/macOS integration is not yet OS-verified.

Desktop retained-directory saves download and extract through one bounded
staging path. The archive and declared payload limits are each 4 GiB, with at
most 100,000 archive entries and 1 MiB transfer buffers. Extraction accepts only
regular files and directories with safe relative paths; links, duplicate paths,
malformed archives and nonzero data after the archive terminator are rejected.
Failure removes the owned staging file and directory without publishing a
partial destination. Native folder publication is atomic and no-replace:
a file, directory or symbolic link created at the chosen destination during
download or extraction is preserved, and the export reports a conflict.
Shared staging cancellation is checked between reads and
writes and before publication; it does not interrupt a blocked network read.
The Files export dialog exposes this cancellation during local saving through
the picker-issued destination identity. It waits for staging cleanup before
allowing a retry or close. Cancellation arriving after atomic publication does
not undo a completed export. Other directory-export dialogs do not yet expose
this local-save cancellation control.

Filesystem edit jobs also support standard plain/primary-MBR FAT16 and the
separate EX5 HD/removable profiles. This is raw
filesystem access, not sampler-object mutation. It uses the same exclusive path
lease, reviewed session revision, streamed journal ranges, input revalidation,
rollback and recovery as SFS. Successful commits refresh the session once;
cancelled or rejected commits that roll back remain retryable at the original
revision. A quarantined interrupted transaction must be recovered before opening
a new session. FAT entry-ID jobs, import review and advertised write capabilities
use this transaction contract.

Normal image opening, Files export and Files editing do not calculate a
whole-image content hash. Before a Files write, native file identity, size and
revision must still match the opened session, including a second check after
freezing the journal and before the first write. After writing, identity and
size are checked without requiring unchanged timestamps. Changed ranges are
read back and the image metadata is reopened and validated before commit.
The application serializes its own writes through path reservations and session
leases. Do not edit an open image in another process: native revision checks
have the host filesystem's timestamp precision and are not a content comparison or
an operating-system-wide exclusive lock. There is no separate unsafe fast mode.

Consumers that explicitly require a content identity, such as retained package
plans, obtain a real SHA-256 lazily under the session read lease. The digest is
cached only for that session revision, with source checks even on cache hits,
and invalidated after commit or rollback. Package application still verifies
the planned content fingerprint. Standalone copy-publishing writers retain
their own source-content checks.

Writable roots advertise `moveEntry` separately from other Files actions.
Move requests use `{ "kind": "MOVE", "entryId": "...", "destinationParentEntryId": "..." }`
inside `images.filesystem.edit`, bound to `expectedRevision`. A move batch contains
only moves to one directory in the same partition; partition roots and protected
metadata cannot be moved. Selected descendants move with their selected ancestor.
Same-parent entries are unchanged. Name collisions reject the whole transaction;
there is no overwrite or directory merge. Payloads and native attributes are
preserved, with directory parent links updated transactionally. Raw moves do not
repair sampler-object relationships. Use the ordinary edit acknowledgement and
job recovery rules; a committed move followed by a failed refresh must not be
resubmitted.

Writable roots advertise `renameEntry` separately from other Files actions.
Submit `{"kind":"RENAME","entryId":"...","newName":"..."}` through
`images.filesystem.edit`. A rename request must contain exactly one edit:
mixing identity-bound edits with path changes is rejected. Rename changes a
file or directory name within its existing parent, never moves or overwrites
another entry. Roots, filesystem metadata and protected entries cannot be
renamed. Native name limits apply, including 8.3 FAT names. FAT edits accept
lowercase ASCII letters and store them in uppercase; case-only renames are
unchanged names and are rejected. SFS names retain their case.
Existing FAT long-name records attached to the renamed entry are retired;
other long-name records are preserved. Payloads, allocation, links and unrelated
metadata remain unchanged. Guarded EX5 media retain their existing capacity
constraints and size. Raw Files rename does not rename embedded sampler objects
or repair their relationships; use Device actions for semantic object renames.

In axkdeck, select one file or directory and choose **Rename...**, the rename
toolbar icon, or **F2**. The prefilled dialog closes after confirmed writing and
refresh, retaining navigation where the renamed entry still matches the view.

`POST /api/v1/filesystem-input-inspections` starts a cancellable read job for
1-10,000 raw `inputs`, each containing one `fileRef` or completed `uploadRef`.
No image session or sampler-object decode is needed. The result preserves input
order as `inputs: [{source, snapshot}]`, where the snapshot contains `revision`,
`sizeBytes` and lowercase hexadecimal `sha256`. Hashing uses bounded reads;
empty and extensionless inputs are valid. Each file must fit the filesystem's
32-bit size field. Host sources require shared path reservations and uploads
are owner-scoped.

Submit that exact reviewed snapshot as `expectedSource` on every `PUT_FILE`.
Execution verifies file identity, size and content before mutation and again
during journal commit validation. A different file with identical bytes still
requires a new review. Changes during commit trigger rollback; changes since
review fail with `filesystem_input_changed`. There is no implicit snapshot
refresh or compatibility path for requests without a snapshot. Upload expiry
or deletion requires acquiring and inspecting the input again.
Repeated references to one input share a retained reader and one verification
before planning and one during commit; conflicting reviewed snapshots are
rejected. SU700 imports verify the complete backing floppy, not each derived
file range separately.

### Floppy Files And Contents

Writable FAT16 roots, including EX5 HD and removable media, advertise
`FAT_FLOPPY_CONTENTS` in `supportedImports`. Dropping only `.ima` or `.img`
files into their Files view opens **File / Contents**, initially **Contents**.
File copies the original image bytes; Contents copies selected filesystem
entries directly into the drop target, preserving their relative hierarchy
without adding an image-name directory. A file-row drop uses its parent.
Mixed drops containing ordinary files or host directories retain raw Files import.

The dialog accepts up to 32 images of 4 MiB each. Each source has its own
selection tree, including recursive directory selection and Select All.
Names are normalized to uppercase and checked against FAT 8.3 limits.
Same-path directories merge; selected files from different images with the
same destination must be renamed or deselected. Existing destination files
default to Skip, with explicit Replace available. Mode changes preserve edits
but invalidate Review. Unreadable Contents remain visible as errors while
File mode remains available. Clean completion preserves Files expansion and
scroll state and closes after cleanup and refresh.

`POST /api/v1/filesystem-image-inspections` starts the read-only
`filesystem.images.inspect` job with `source: {fileRef: ...}` or
`source: {uploadRef: ...}`. It enumerates bounded FAT image contents without
sampler-object conversion, including auxiliary and configuration files.
Results contain `inspectionToken` and `entries`, each with `entryId`,
`relativePath`, `directory` and `snapshot` (null for directories).
Each inspection accepts at most 8,192 entries and 4 MiB of aggregate file data.

For each selected file, submit
`source: {imageEntryRef: {inspectionToken, entryId}}` and its exact snapshot
as `expectedSource` through the ordinary Files edit job. These references are
valid only for Files edits, not general input or upload APIs. Readers retain
immutable contained bytes and verify the original image identity and content
before mutation and during commit validation. Host files remain read-only;
uploads retain their owner-scoped leases.

Inspection handles belong to their authenticated owner and expire after
15 minutes. `POST /api/v1/filesystem-image-inspections/release` accepts
`{inspectionToken}` and idempotently releases an unused handle. Active writers
retain their readers through completion even after handle release or expiry.
The service admits at most 32 pending or retained inspections, including
released handles still held by writers. Expired or changed sources require a
new inspection; existing transaction, revision and rollback rules still apply.

### SU700 Floppy Import

Filesystem root capabilities advertise `supportedImports: ["SU700_FLOPPY"]`
only for writable SFS roots containing a recognized existing SU700 volume.
Empty or unidentified SFS roots do not imply SU700 support.

`POST /api/v1/su700-import-inspections` starts the cancellable
`images.su700.import.inspect` job. Supply a file/upload `source`, nullable
`destination`, and nullable `includedExtras`. A destination specifies
`imageId`, `expectedRevision`, `rootEntryId`, and a new `volumeName` of 1-16
printable ASCII characters without path separators or edge spaces.
Source-only inspection returns `COMPLETE`, `UNRELATED`, or `UNSUPPORTED`,
the source snapshot, song/sample counts, and destination-relative file paths.
Destination inspection additionally checks collisions and available allocation
space without writing. `includedExtras: null` includes all unreferenced files;
an explicit array selects their source paths.

`POST /api/v1/su700-imports` starts `images.su700.import` with the same fields,
a non-null destination, and the reviewed `expectedSource` snapshot. Execution
revalidates source identity/content and destination revision, then uses one
atomic filesystem transaction. Submit a stable idempotency key; recover an
uncertain submission using that same key rather than issuing another import.

Only complete, flat FAT12 floppy images are supported. Control bytes, payloads,
and stored eight-byte basenames are preserved, including spaces before `.SSQ`
and `.SSP`. Songs go to `SUSQ`, samples to `SUSP`, and control/selected extra
files to the new volume root. Missing or incomplete payloads and ambiguous
references fail before mutation. Existing volumes are never merged/replaced;
multi-disk reconstruction and SU700 image creation are not supported.

### Generic Filesystem Import

`POST /api/v1/image-filesystem-import-inspections` starts the cancellable
`images.filesystem.import.inspect` read job for an admitted SFS or FAT16 destination. Supply
`imageId`, `expectedRevision`, `parentEntryId` and 1-10,000 ordered `entries`.
Each entry contains `relativePath` components, `directory`, `sizeBytes` and
optional `conflict` (`SKIP` or `REPLACE`). Directories have zero size and must
precede their children. The destination is resolved through the owned session;
stale image revisions and protected destinations are rejected.

The result retains the destination/revision and entry order. Each entry adds
`action`, nullable `existingSizeBytes` and `issue`; `conflictCount` counts
blocking rows. Actions are `CREATE_DIRECTORY`, `MERGE_DIRECTORY`, `CREATE_FILE`,
`SKIP_FILE`, `REPLACE_FILE` and `CONFLICT`. Existing directories merge;
file/directory collisions remain blocking. File collisions, including an earlier
incoming entry at the same path, follow the requested Skip/Replace policy.
Earlier incoming files are included in the reported existing size and identified
in the issue text. Directory aliases share collision state; replacing a file
alias does not replace its other names.

This is path/type review, not an allocation reservation or source inspection.
The job reads filesystem metadata without importing payloads or mutating the
image. It bounds cached directory/path entries to 250,000 and fails rather than
returning a partial review when that limit is exceeded. Execution still requires
the reviewed input snapshots, validates allocation and rechecks the image revision.
The GUI session-bound import driver exposes both review jobs and their
observation/cancellation paths. Add files uses the shared batch storage picker
or local file chooser with bounded sequential `FILE` uploads, including empty
and extensionless files. Filename and Skip/Replace changes invalidate the
destination review, and confirmation submits the complete ordered batch with
its exact input snapshots. At most 100 editable rows are rendered per review
page without truncating the batch. Blocking conflicts and all-skipped batches
cannot submit. A known job with an unconfirmed outcome is observed, never
resubmitted; failed writes require refresh and committed writes whose refresh
failed only retry refresh. Temporary uploads are released after closing or
known completion. An unconfirmed write retains its uploads until its outcome
is known or they expire, so closing the dialog cannot remove pending inputs.

Import from disk uses the shared readable-directory picker and traverses its
contents through paginated sandbox listings. It imports regular files and
directories, including empty child directories, without wrapping them in the
selected source folder's name. Symlinks, reparse points, special files and names
not representable by the sandbox listing are excluded. The review is a captured
selection, not a live directory synchronization; every included file still
requires its independently checked source snapshot. An empty selection cannot
submit. Failed traversal never returns a partial selection.

Traversal is iterative and bounded to 10,000 entries, fewer than 1,024 path
components and 4,194,304 cumulative source/destination path characters. It checks
page identities, immediate-child paths, duplicate entries and cursor progress;
cancellation or image navigation stops acquisition. Directory rows merge by
default. Renaming a directory updates only its own descendants, including when
several source directories are explicitly renamed to the same destination.
All entries are reviewed and submitted together, with parents before children.
Client-side folder drops use the separate bounded browser/native readers
described above. Native OS directory-picker acceptance remains a separate check
from this workspace-directory workflow.

Raw file export inspection and execution operate independently of object
extraction.
It accepts owned image-session entry IDs and a reviewed revision. Selected
directories include their descendants and empty directories; selecting an
ancestor and its child exports that subtree once. Structural metadata is omitted
from a subtree with a notice and cannot be selected directly for export.

Inspection reports stored source paths, output path components, sizes and
host-name changes. Invalid host-name characters are replaced, Windows device
names are escaped, and ASCII case-insensitive output collisions are rejected.
Execution streams exact file bytes in at most 1 MiB chunks through structured
SFS, FAT or ISO locators, without interpreting sampler objects. It holds the
session read lease and destination reservation, verifies source identity and
content before publication, and uses the sandbox publisher for a new or empty
destination. Cancellation or a failure before publication leaves that destination
unchanged and removes temporary output. The shared publisher checks cancellation
between copy chunks and before committing the destination.

`POST /api/v1/image-filesystem-export-inspections` accepts `imageId`,
`expectedRevision`, `entryIds` and `layout` and returns `rootDirectory`, the expanded
entries, notices and `totalBytes`. `layout` is required:

- `EXPORT_FOLDER`: after removing duplicate and descendant selections, a single
  directory or partition becomes the destination root. `rootDirectory` describes
  that root; `entries` contains only its descendants, with paths relative to the
  chosen destination. An empty root is exportable even when `entries` is empty.
  Single files and multiple independent selections retain their names inside the
  destination, with `rootDirectory: null`.
- `SELECTED_ENTRIES`: preserve selected entry names as top-level paths, including
  selected directory names. `rootDirectory` is null. Native drag-out uses this
  layout so the operating system receives the selected folders themselves.

For example, exporting `AMENBOSH` to a folder named `Renamed` writes
`Renamed/SONGCONT.DAT`, not `Renamed/AMENBOSH/SONGCONT.DAT`. The preview tree shows
this same structure. Native and server-workspace destinations share this policy.

`POST /api/v1/image-filesystem-exports` adds a `destination` and
submits an idempotent export job. Its result includes the same export summary:

- `WORKSPACE`: `output` is a sandbox `DirectoryRef`. The job reserves the entire
  destination exclusively and publishes a new/empty directory.
- `DOWNLOAD`: `directoryName` supplies the retained TAR basename. The existing
  download store enforces ownership, retention, archive quotas and its TAR path
  limits. `download.contentPath` provides the authenticated download endpoint.

Both routes use the same verified staging service. Download creation observes
job cancellation while building the archive. An image revision change requires
a new inspection rather than silently changing the reviewed revision. Export
does not modify the image or repair sampler relationships.

The Files review uses a bounded, expandable tree with paged rows and a fixed
header and footer. Expansion does not change the export selection. Clean exports
close after publication and archive cleanup, reporting success in the workspace
status. Notices and cleanup warnings remain visible with Done; failed exports
retain their recovery actions.

The GUI destination review supports workspace and managed-local destinations;
desktop drag exports use the same retained-download route. Filesystem root flags
describe mutation capabilities; operation availability is exposed through
the existing operation registry.

Disk images and durable outputs belong to the server filesystem. The server
persists named workspaces selected by an authenticated operator. API requests
identify entries with a root ID and a normalized relative path; they never send
an absolute server path:

```json
{
  "rootId": "workspace",
  "relativePath": "images/library.hds"
}
```

The server rejects absolute paths, traversal, and links that escape a workspace.
Clients discover roots with `GET /api/v1/roots` and browse them with bounded
directory requests.

A first launch is valid with no workspace. `GET /api/v1/workspaces` then reports
`NO_AVAILABLE_WORKSPACE`; normal file operations remain unavailable until a
workspace is added. The authenticated setup API can enumerate host directories,
but never files, while choosing a workspace. This temporary broad view uses the
server process's operating-system permissions. Once a directory is committed,
all normal reads and writes return to the relative-path sandbox above.

An independently launched server stores workspace configuration per user at:

- `$XDG_CONFIG_HOME/tphzz/axklib-server/workspaces.json`, or
  `~/.config/tphzz/axklib-server/workspaces.json`, on Linux;
- `%APPDATA%\tphzz\axklib-server\workspaces.json` on Windows; and
- `~/Library/Application Support/tphzz/axklib-server/workspaces.json` on macOS.

Axkdeck owns the local server process it launches and passes a distinct sidecar
registry explicitly. That registry is
`tphzz/axkdeck/axklib-server/workspaces.json` below the platform configuration
root. Consequently, a standalone local server can run with its own workspace
configuration without modifying axkdeck's sidecar configuration. A remote
server likewise keeps its registry on the remote host; axkdeck does not copy or
reinterpret it.

The earlier unreleased filenames and locations are not read or migrated.

Use `--workspace-store PATH` or the JSON `workspaceStore` setting for a
deliberate override. Missing directories remain in the registry with an
availability error so they can be repaired or removed. A corrupt store is not
overwritten automatically; the recovery endpoint archives it before creating
an empty replacement.

Workspace mutations use the snapshot `revision` as an optimistic concurrency
check. A new, disjoint workspace can be added while image sessions or jobs are
active. Removing or relocating a workspace while an image session or active job
uses that specific workspace returns a conflict; unrelated workspaces remain
configurable. Closing the image or waiting for the job to finish releases that
workspace. Workspace directories must not be identical, ancestors, or
descendants of one another, because each root is an independent reservation and
sandbox boundary.

Temporary uploads support browser-selected audio, MIDI, portable packages,
JSON manifests, supported media inputs and raw Files inputs. Raw inputs use
kind `FILE` with `application/octet-stream`; arbitrary extensions and empty
files are accepted. Typed audio/media/package upload restrictions remain in
force. A client creates an upload, streams bounded chunks, and
completes it before using its `UploadRef`. An operation can consume an upload
only where its request schema explicitly permits one. Source disk images use a
server `FileRef`, not an upload.

WAV, SFZ, report, package, and image outputs are written to caller-selected
server `FileRef` or `DirectoryRef` destinations. They remain after the job
record expires. `GET /api/v1/files/content` provides an authenticated streamed
download, including one bounded byte range, when a user explicitly wants a
server file on the client machine. `HEAD` returns the current quoted revision
as `ETag`. A ranged `GET` must send that value in `If-Match`; a changed file is
rejected instead of returning bytes from mixed revisions. Downloads hold the
same shared path reservation used by image sessions while reading.

For an explicit directory download, `POST /api/v1/files/archive` accepts a
`DirectoryRef` and returns a job resource. The read-job executor creates the
bounded, owner-scoped TAR snapshot in temporary server storage without
occupying an HTTP worker. The terminal job result contains its authenticated
content path and short expiry. Download the archive, then delete that content
resource; expiry and startup cleanup are fallbacks. Archive creation is
cancellable and rejects links, non-regular entries, source changes, excessive
entry counts, and byte-quota overflow. It does not move, modify, or take
ownership of the source directory or any durable job output. Archive content is
sent from the retained file in bounded transport chunks rather than copied into
a response-sized memory buffer. The
`maximumConcurrentArchiveDownloads` configuration limit, which defaults to
`1` and accepts values from `1` through `64`, bounds simultaneous transfers.
Expiry and explicit deletion defer removal while a transfer lease is active.

## Loopback Use

Every route except liveness requires bearer authentication, including
loopback. Start a standalone loopback server with a token:

```bash
axklib-server \
  --token 0123456789abcdef0123456789abcdef
```

The default endpoint is `http://127.0.0.1:7331/api/v1`. Use `--port 0` to let
the operating system select a free port. Axkdeck sidecar mode does this and
generates a high-entropy token automatically. Endpoint metadata is exchanged
through an owner-only connection file and removed after axkdeck consumes it;
the token is not passed on the sidecar command line. Connection-file sidecar
mode rejects `--config`, ignores environment configuration, rejects
caller-supplied token options, and always generates a new token so headless or
machine-wide LAN configuration cannot change the child process's trust model.
An owning application may also pass `--parent-pid PID` together with
`--connection-file`; the server then exits when that process no longer exists.
Standalone servers omit this option and are unaffected.

## Configuration

Configuration precedence is deterministic:

1. compiled safe defaults;
2. a strict JSON configuration file;
3. named environment overrides; and
4. command-line options.

Select a file with `--config PATH` or `AXKLIB_SERVER_CONFIG`. Unknown JSON keys
and incorrectly typed values are errors. A LAN configuration can be written as:

```json
{
  "bindAddress": "0.0.0.0",
  "port": 7331,
  "allowInsecureRemoteHttp": true,
  "tokenHashes": [
    {
      "principalId": "studio",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  ],
  "allowedOrigins": ["https://sampler.example.test"],
  "workspaceStore": "/var/lib/axkdeck/workspaces.json",
  "stateDirectory": "/var/lib/axklib-server"
}
```

The scalar environment overrides are `AXKLIB_SERVER_BIND`,
`AXKLIB_SERVER_PORT`, `AXKLIB_SERVER_TOKEN`,
`AXKLIB_SERVER_STATE_DIRECTORY`, `AXKLIB_SERVER_WORKERS`,
`AXKLIB_SERVER_JOB_WORKERS`, `AXKLIB_SERVER_WRITE_JOB_WORKERS`, and
`AXKLIB_SERVER_MAX_QUEUED_JOBS`. Prefer the configuration file for the
workspace-store override, origins, token hashes, and detailed resource limits.

Directory archives count both files and directories. In addition to entry and
byte quotas, `maximumDownloadArchiveDepth` (default `64`) and
`maximumDownloadArchivePathBytes` (default 32 MiB) bound traversal depth and
the aggregate bytes needed for relative paths. Traversal retains only the
selected directory capability; files are reopened and identity-checked one at
a time while the TAR is written, so `maximumDownloadArchiveEntries` is not also
a file-descriptor budget.

Fresh floppy and ISO planning uses `maximumMediaBuildObjectBytes` (default
64 MiB), `maximumMediaBuildPayloadBytes` (default 737,280,000 bytes), and
`maximumMediaBuildOutputBytes` (default 737,280,000 bytes). The first limit
cannot exceed the aggregate payload limit. Server values may lower, but cannot
raise, the public engine defaults. Payload admission failures return HTTP `413`
during planning; an oversized ISO projection is rejected during apply before
the temporary file is resized or published. These values are reported by
`GET /api/v1/system/capabilities`.

In-place image mutations are protected by an `AXKJNL02` alteration journal.
The journal stores both the original and replacement bytes for every changed
extent, so its exact size is approximately twice the changed payload plus
metadata. `maximumAlterationJournalBytes` defaults to 4,362,076,160 bytes,
which covers a complete rewrite at the supported 2 GiB image boundary plus
64 MiB of metadata, and may be configured up to 8 GiB. Journal publication,
application and recovery use bounded streaming I/O; the configured limit is a
storage and admission bound, not a request to allocate that amount of memory. Before
mutating an image, the server verifies both the exact encoded journal size and
available space in the state directory. Capacity failures leave the target
image unchanged and report the required and configured or available byte
counts. The active limit is reported by
`GET /api/v1/system/capabilities`.

Journal patches accept owned metadata buffers or shared reader-backed ranges.
All ranges are frozen on disk before mutation; apply and rollback read those
frozen bytes, not potentially changed input files. Overlapping or out-of-range
patches are rejected before writing. Each read/write chunk is bounded to at most
1 MiB. Cancellation during application restores the original bytes before
returning; interrupted transactions retain their normal startup recovery path.
Cancellation during final validation also rolls back before the commit marker.
Rollback and recovery flush and read back the restored ranges before removing
the journal. Failed restoration verification retains the journal and blocks
further writes pending recovery.

The application listener is plaintext because Crow TLS is intentionally not
enabled. Non-loopback startup is therefore rejected unless
`allowInsecureRemoteHttp` or `--allow-insecure-remote-http` is explicit, in
addition to named token hashes and non-wildcard origins. The flag acknowledges
that bearer credentials are exposed on the application connection; it does not
make HTTP secure. Keep the listener loopback-only or place it on a private
backend behind a trusted HTTPS reverse proxy. Never expose plaintext remote
mode directly to an untrusted network.

## Operations And Jobs

`GET /api/v1/system/capabilities` is the runtime operation catalogue. It exposes
domain routes, execution modes, request and result schemas, and shared-route
variants. Use this response and the [OpenAPI reference](openapi.md) to discover
the operations available from the running server.

Short bounded reads return directly. Scans, extraction, package writes, image
creation, and alteration return a job resource. Use REST to inspect or cancel a
job. For live updates, request a short-lived single-use ticket from
`POST /api/v1/event-tickets`, then connect to `/api/v1/events` with the
`axklib.events.v1` subprotocol. Events have monotonically increasing per-job
sequence numbers. After a disconnect, replay retained events through REST and
fetch the job snapshot; WebSocket delivery is an update hint, not the source of
truth.

`POST /api/v1/images` requires one explicit `ImageSourceRef`. Use
`{"kind":"FILE","file":...}` for an image or standalone object file and
`{"kind":"AXK_OBJECT_DIRECTORY","directory":...}` for a flat directory of
Yamaha object files or a parent containing one level of related disk folders.
The parent form assembles complete contiguous multi-disk Wave Data segments.
Opening a flat leaf does not search sibling folders. This keeps image opening
and directory navigation bounded to the selected source.
Object-directory sessions are bounded and read-only: inventory, relationships,
preview, audition, and package export are available, while image alteration and
package import are not. An incomplete leaf can be inventoried, but split Wave
Data cannot be previewed, auditioned, or exported as a complete package until
its companion folders are attached.

`POST /api/v1/images/{imageId}/companions` accepts `expectedRevision` and a
`selection`: `SOURCES` with a list of `ImageSourceRef` values, or
`IMMEDIATE_SIBLINGS` for an explicit nearby search. Directory sources use
`AXK_OBJECT_DIRECTORY`; raw floppy members use `FILE`.
Cataloged folders expose `floppySet` with the required disk index at open time.
Attachment validates the same-label, contiguous member sequence and admits all
cataloged objects from later members, not only waveform continuations. An
incomplete attachment remains `INCOMPLETE`; playback and export retries wait
for `COMPLETE`. A wrong, ambiguous, or invalid attachment leaves the current
session unchanged. No sibling search runs automatically.

Catalog-less or invalid-catalog folders retain `RECOVERY` behavior. When an
explicit operation needs more Wave Data, the server checks only the requested
directories and admits exact continuation segments
with normalized Yamaha header identities, even when Yamaha changes the host
filename between disks, plus Wave Data objects whose embedded names exactly
satisfy active unresolved Sample member lanes. The image ID and object IDs
remain stable, while the session revision advances. Attachments are retained
only for the lifetime of that image session; the source folders and their files
are never merged or modified.

Axkdeck selects companion sources directly through the server storage picker,
including on localhost. No local/remote source chooser is inserted. The picker
starts beside the opened source unless a previous companion location is retained.

`POST /api/v1/files/list` performs only a bounded directory listing. A media
picker navigates directories without media inspection and inspects the current
directory with `POST /api/v1/files/media-source/inspect` only when the user
chooses to open it. The
response reports `mediaSourceKind: "AXK_OBJECT_DIRECTORY"` when bounded
file-prefix inspection recognizes Yamaha object data, otherwise `null`.
Inspection does not decode complete payloads or recurse beyond one related
disk-folder level.

### Image browsing size accounting

Image object responses expose `sizeBytes` as the complete logical stored size
of that object file or SFS record, including its object metadata and stored
payload. Programs, Sample Banks, and Samples also expose
`sizeWithDependenciesBytes`. This is an exact, deduplicated forward-closure
total using the same known relationship requirements as portable-package
export: a Program includes its assigned Sample Banks and Samples, a Sample Bank
includes its member Samples, and a Sample includes its left and right Wave
Data. The value is `null` when any required relationship is missing,
ambiguous, cyclic, or otherwise unavailable; clients must not present an
incomplete lower bound as an exact total.

Volume content nodes expose nullable `sizeBytes` as the deduplicated sum of all
contained object sizes. These values describe logical stored bytes and apply to
all readable media. They do not include SFS cluster rounding, allocation slack,
or filesystem support structures and therefore must not be used as physical
free-space or import-capacity figures.

### Image integrity and mutation admission

Every readable image session advertises `images.validation.issues`. Retrieve
the bounded issue list with
`GET /api/v1/images/{imageId}/validation/issues`. An SFS image remains readable
when its two allocation bitmap copies differ, a stored bitmap disagrees with
index extents, or clusters have multiple owners; inventory, validation, preview,
audition, and supported exports remain available for still-readable objects.

Those allocation errors make alteration unsafe. The session then omits
`images.alter.volumes`, `images.alter.partitions`, `images.alter.objects`,
`images.package.import`, orphan cleanup, and generated-Program operations from
`availableOperations`. Mutation admission checks the same predicate again under
the session lease and returns `image_integrity_unsafe` if inconsistent
allocation metadata is encountered. Clients must treat capability omission and
that error as a safety boundary, not as a free-space failure.

An otherwise allocation-clean, file-backed SFS session whose only allocation
errors are repairable extent byte-total mismatches advertises
`images.extent_layout.repair`. The operation accepts the current image identity,
expected revision, and a workspace or retained-download destination. It repairs
all eligible records together into a distinct image copy, verifies both bitmap
copies and reconstructed ownership, and verifies each repaired logical payload
before publication. It never alters the open source image. The operation is not
advertised for bitmap divergence, stored/index disagreement, invalid extents,
cross-links, or an ambiguous underreported multi-extent record.

Writable SFS image sessions advertise `images.alter.objects`. Use
`images.deletion.inspect` with the image ID, expected revision, target object
IDs, and explicit optional-cleanup object IDs to obtain the complete deletion
impact. Targets may be Programs, Sample Banks, Samples, or Wave Data and may
span volumes and partitions. The inspection marks each target eligible or
blocked; `canApply` is true when at least one requested target can be deleted
safely. Submit the unchanged reviewed selection to `images.delete`. Eligible
targets are applied atomically while blocked target IDs are returned unchanged
in `blockedObjectIds`.
Deletion is a write job: clients must wait for a terminal job snapshot, then
refresh the retained image session. The delete operation replans at admission
and the underlying alteration revalidates the image under the mutation lease,
so an outdated revision or changed relationship graph fails without publishing
a partial result.

Writable SFS sessions also advertise `images.deletion.orphans.inspect`.
Pass one volume content-scope ID with the image ID and expected revision to
discover Wave Data that the current relationship graph classifies as confirmed
unreferenced. The bounded response returns at most 1,024 candidates, their
locations, stored sizes, and recoverable allocation; `totalCandidateCount`
indicates whether another cleanup pass may be needed. This operation is
discovery only. Before deleting a selected subset, submit those opaque object
IDs to `images.deletion.inspect` and require every target to remain eligible,
then use the normal `images.delete` job. Re-run both inspections immediately
before mutation so a stale UI list cannot authorize deletion.

Writable SFS sessions also advertise `images.package.import`; every readable
media session advertises `images.package.export`. Import is a three-step
operation: inspect the package, request an owner-bound revision-specific plan,
then submit its token as a write job. Release an abandoned token explicitly.
The apply path uses the same journaled in-place mutation boundary as other
session alterations and refreshes the retained session only after validation.

Session package export is a read job. Its `roots` array accepts one to 1,024
exact volume, Program, Sample Bank, Sample, or Wave Data roots. Object roots
use the opaque IDs returned by the retained image session; the server resolves
them under the same revision-bound read lease. A volume root is
`{"kind":"VOLUME","contentId":"..."}` and uses the volume's opaque content
ID; the obsolete partition-index and visible-name selector is rejected. A
single root receives its
specific package extension. Multiple roots of the same kind keep that typed
extension; only mixed root kinds produce `.axkpkg`. A `WORKSPACE` destination
publishes through the normal sandbox. A `DOWNLOAD`
destination retains the package in private owner-scoped storage and returns an
authenticated content path with a short expiry. The client streams that
content and deletes the retained resource when the save completes; expiry is
only a fallback.

A3K sessions are read-only and expose one synthetic volume. They support
selected-object package export and direct whole-volume `.axkvol` export through
the generic session operation. They do not advertise package import, mutation,
repair, media conversion, floppy export, or partition-level batch-volume
export.

Writable SFS sessions advertise package import on both volumes and partitions.
A volume import plans one or more package roots into that existing volume. A
partition batch import accepts one to 256 `.axkvol` inputs, derives one new
volume from each package's placement hint, resolves duplicate hints with
deterministic suffixes, and returns per-volume object counts and final names for
preview. Edited names are submitted as indexed overrides in a replacement plan.
Applying the accepted token creates the complete set in one journaled image
mutation and one session revision; any failure rolls back every volume.

Writable A-series SFS sessions also advertise `images.floppy.import`. Submit
one FAT floppy or up to 32 companion images to `images.floppy_import.inspect`
using admitted `DISK_IMAGE` uploads or workspace file references. The completed
job identifies A-series, SU700, TX16W or unrelated content. A-series results
include an owner-scoped inspection token, disk-set completeness, selectable
objects, required dependencies, and excluded configuration or auxiliary files.
Incomplete sets cannot be imported; unrelated disks cannot form a batch.

Call `images.floppy_import.plan` with the inspection token, selected object
keys, destination, image ID and expected revision. It uses the same conflicts,
Program slot placement and opaque Sequence decisions as package import, without
requiring an intermediate archive. Apply its token with `images.floppy_import`;
the existing transaction publishes the complete selection in one revision.
Source changes, expired inspections and stale destination revisions are rejected.
Release unused inspections with `images.floppy_import.release` and unused plans
with `images.package_import.release`. Inspections expire after 15 minutes;
each source is limited to 4 MiB and retained inspections have a shared 512 MiB
reservation budget. SYSTEM and SYSTEM2 files are not imported as sampler objects.

SFS and ISO9660 sessions additionally advertise
`images.volume_package_export` on partition-like content nodes. First call
`images.volume_package_export.inspect` with the image ID, expected revision,
and partition or CD-ROM group `scopeId`. The bounded inspection lists only its
immediate volumes, reports which are empty, and assigns deterministic package
paths. The read job builds every nonempty volume against one shared catalog and
relationship graph but publishes each result as a separate `.axkvol`. It writes
`volume-packages.axklib.json` beside the packages, skips empty volumes, and
records per-volume closure failures without discarding successful packages. A
run with zero successful packages publishes nothing. `WORKSPACE` creates one
new no-overwrite directory; `DOWNLOAD` returns the same directory as a retained
TAR for desktop extraction.

File-backed SFS sessions additionally advertise
`images.volume_floppy_export` on partition content nodes. First call
`images.volume_floppy_export.inspect` with the image ID, expected revision,
and exact partition `scopeId`. The inspection resolves every immediate volume
against one shared catalog and relationship graph. Each item is `READY`,
`EMPTY`, or `BLOCKED` and reports its object count, projected floppy count and
raw byte size, collision-safe directory name, and structured issues.

Start `images.volume_floppy_export` with the inspected partition and either a
`WORKSPACE` or `DOWNLOAD` directory destination. Every successful volume is
written to its own subdirectory with raw members named `disk01.ima`,
`disk02.ima`, and so on; the members use the exact same planner and bytes as an
individual floppy export. `volume-floppies.axklib.json` at the root records
exported disks and their sizes and SHA-256 digests, skipped empty volumes,
blocked volumes, and runtime failures. Processing is sequential and partial:
a failed volume directory is removed while earlier and later successful
volumes remain. A run with no successful volume publishes nothing.

A `WORKSPACE` destination creates one new no-overwrite directory. A
`DOWNLOAD` destination retains the same directory as a TAR, which the desktop
extracts through its directory chooser before deleting the retained resource.

File-backed SFS sessions also advertise `images.media_conversion`. Use
`images.media_conversion.inspect` before starting the read job. The inspection
accepts exactly one of these scopes:

- `ISO9660` with a zero-based `partitionIndex` converts the complete selected
  partition to one Yamaha CD-ROM image.
- `FAT12_FLOPPY` with a zero-based `partitionIndex` and the stable
  `volumeDirectoryId` returned by the content tree converts the complete
  selected volume to Yamaha FAT12 media. A volume that fits produces one
  1,474,560-byte `.ima`; a larger admitted volume produces an ordered
  multi-floppy `.zip` containing two through 32 images and a manifest.

The inspection reports the selected volumes, object and payload counts,
projected output size, capacity, `artifactKind`, `outputExtension`,
`floppyImageCount`, a suggested filename, and structured issues. Conversion
never drops objects to make a selection fit. Complete objects remain byte
identical; only oversized Wave Data is divided into exact Yamaha continuation
segments. A `WORKSPACE` destination publishes the inspected `.iso`, `.ima`, or
`.zip` through the sandbox. A `DOWNLOAD` destination uses the same private
owner-scoped retained-file flow as package export. Multi-floppy inspection also
reports that load and audition are hardware-verified while sampler save/reload
validation remains pending.

Each WebSocket connection has bounded lifetime delivery budgets for both event
count and serialized bytes. The defaults are 1,024 events and 4 MiB. When
either budget is exhausted, the server closes the connection with status 1013;
the client obtains a new event ticket, reconnects, replays from its last
sequence number, and reconciles the job snapshot. Configure the budgets with
`maximumWebsocketDeliveryEvents` and `maximumWebsocketDeliveryBytes`. This
connection rollover keeps Crow's asynchronous outbound queue bounded even when
a client stops reading.

Transient capacity errors return HTTP `429`, set `error.retryable` to `true`,
and include `Retry-After`. Clients should delay and retry or release an idle
resource. A request whose own archive or payload exceeds a configured limit is
not transient and returns `413` instead.

The capabilities response also reports the active JSON, upload, download,
archive traversal, media build, queue, image-session, and page limits. Clients
should honor those values rather than assuming compiled defaults.

Existing HDS images are normally altered into a distinct output file. A trusted
workspace client that needs to update the selected image may submit
`replaceSource: true` to `alter.hds` and set `output` to the same `FileRef` as
`source`. It must close active image sessions first. The application writes and
validates a temporary sibling before atomically replacing the source; this mode
does not permit a separate `overwrite` request. Clients may call
`alter.inspect` first for advisory validation, but the inspection does not
create an apply token or reserve the destination. Every `alter.hds` job request
contains the complete source, manifest, input bindings, and output.

## Low-Concurrency Deployment Profile

For a 64-bit Raspberry Pi 4 or newer with at least 4 GiB of memory, start with
two Crow threads, one application job worker, and one serialized write worker.
The following configuration keeps network and queue concurrency bounded while
still allowing one long-running image operation:

```json
{
  "workerThreads": 2,
  "jobWorkerThreads": 1,
  "writeJobWorkerThreads": 1,
  "maximumQueuedJobs": 8,
  "maximumRetainedJobs": 128,
  "maximumImageSessions": 2,
  "maximumUploads": 4,
  "maximumUploadTotalBytes": 1073741824,
  "maximumWebsocketDeliveryEvents": 128,
  "maximumWebsocketDeliveryBytes": 524288
}
```

Add authentication, origins, and the state directory described above;
the fragment is not a complete server configuration. Keep the state directory
on storage with enough free space for the configured upload total and one
maximum-size alteration journal. Increase workers only after measuring the
actual image and extraction workload. A single large domain operation can
require substantially more memory than the HTTP transport, so the transport
budget is not a whole-image memory promise.

The maintained loopback profile enforces these broad release-build budgets:

- `system.version` REST p95 at or below 50 ms;
- at least 100 requests per second at concurrency eight;
- no more than 16 MiB resident-memory growth after the request sample;
- no more than eight additional file descriptors; and
- no more than 64 KiB temporary storage for the read-only sample.

The profile records the direct application-service cost alongside REST so
transport overhead is visible rather than attributed to domain algorithms. Run
it with:

```bash
ctest --test-dir build/native/release -R '^Server\.PerformanceProfile$' --output-on-failure
```

The path-neutral report is generated at
`build/native/release/apps/server/server-performance.json`. Cancellation and
sidecar shutdown are independently bounded by the resilience test at two and
five seconds respectively. `Server.ParentProcessLifetime` separately verifies
that a sidecar exits within two seconds after its owning process disappears.

## Resilience Validation

`Server.ResilienceIntegration` runs two authenticated principals against small
configured limits. It covers authentication non-disclosure, traversal,
reserved upload/session/job quotas, malformed JSON, sparse ranged reads,
slowloris and slow-upload sockets, concurrent requests, cancellation, restart
cleanup, log redaction, and preservation of completed outputs.

Failure coverage is divided by boundary:

| Failure boundary | Maintained validation |
| --- | --- |
| Capacity allocation | `UploadStoreTest.ConcurrentReservationsCannotExceedTheWorkspaceQuota`, `DownloadArchiveStoreTest.ConcurrentReservationsCannotExceedTheArchiveQuota`, and constrained server queue/session admission |
| Filesystem cleanup | `DownloadArchiveStoreTest.RetainsExpiredArchiveAndQuotaWhenRemovalFails`, sandbox link/traversal tests, and resilience restart cleanup |
| Progress/event callback | `JobManager.IgnoresRegressingProgressWithinAPhaseAndContainsSubscriberFailures` and bounded event-dispatcher tests |
| Socket/disconnect | resilience slow-client sockets plus loopback WebSocket disconnect, replay, ticket-reuse, and delivery-budget tests |
| Atomic publication | extraction strict/cancellation tests and build/alteration cancellation at every mutation phase |

ASan/UBSan and TSan CI jobs run the application and server suites. The
`server-fuzz` CI job compiles the production JSON request validator with Clang
and runs its bounded seed-corpus smoke. The same fuzz smoke can be run locally
when Clang with libFuzzer is installed:

```bash
cmake --preset fuzz-local
cmake --build --preset fuzz-local --target axk_server_request_validation_fuzz_smoke
```

## Operator Diagnostics

`GET /api/v1/system/health/live` is an unauthenticated process liveness check.
Readiness and `GET /api/v1/system/metrics` require authentication. Readiness
reports configuration, sandbox, writable workspace, state-storage cleanup,
upload cleanup, and executor-admission checks separately and returns `503` when
any required check is unavailable. Failed upload deletion retains both the
entry and its quota reservation. Readiness remains unavailable until cleanup
succeeds; `.upload` files recovered at startup are tracked as orphans rather
than silently ignored. Metrics expose cleanup health, failed deletion count,
orphan file/byte counts, and reserved upload bytes alongside bounded aggregate
request counters. They contain no request or payload data.

Request logs are one JSON object per line. They contain only the request ID,
HTTP method, URL path without its query, response status, and elapsed time.
Authorization headers, request bodies, sandbox paths supplied through query
parameters, and payload content are never logged.

## API Contract

The complete OpenAPI 3.1 document is available from authenticated running
servers at `GET /api/v1/openapi.json`. Release installations also include the
static document under `share/axklib/server/openapi-v1.json`, so client tooling
does not need a live server to read the contract.

The source distribution checks in the same complete document at
`apps/server/contracts/openapi-v1.json`. It includes both protocol
infrastructure and every operation expanded from the application registry.
The adjacent `openapi-v1.base.json` is only the schema and infrastructure input
used by the native contract generator; client generators must not consume that
partial base. Native builds compare a fresh registry expansion byte-for-byte
with the complete checked-in document. Axkdeck likewise regenerates its
TypeScript declarations from the complete document and rejects stale generated
types.

JSON fields use lower camel case and enum values use upper snake case. Every
HTTP response includes `X-Request-Id`; a caller may supply a request ID that
matches the bounded contract, otherwise the server generates one. Collection
pages use a bounded `limit` and an opaque cursor. Clients must not parse or
construct cursor values.

Storage directory listings put directories before files and use English Unicode
natural ordering: case and accent differences are ignored for the primary
comparison, numeric sequences sort by value, and punctuation remains significant.
Original UTF-8 names break ties, so distinct spellings are never merged. Names
and paths are returned unchanged. The workspace-folder browser uses the same
name ordering. Ordering does not depend on the server or client OS locale.
Pagination uses this complete ordering; clients retain the returned order rather
than sorting individual pages. Cursors are navigation markers, not directory
snapshots, and directory changes between requests may change the available entries.

`images.preview` accepts either a Wave Data (`SMPL`) or Sample (`SBNK`) object
identifier. A Wave Data preview returns one `MONO` lane over its physical PCM
extent. A Sample preview applies its member start and length fields and returns
one `LEFT` lane plus an optional `RIGHT` lane. Each lane identifies its source
Wave Data and its own frame count; the response-level frame count is the
playback timeline used for audition and playhead positioning.

`auditions.prepare` accepts up to 256 ordered, unique Sample or Wave Data object
identifiers. It validates the complete selection before retaining one bounded
audition bundle. Every clip exposes one or two mono-WAV lane ranges in
`/auditions/{auditionId}/content`; lane sample rates and decoded widths may
differ and clients normalize them independently. The default aggregate content
limit is 128 MiB. A failure rejects the complete request and includes the
responsible object ID when one object caused it.

The optional `sourceWindow` is `PLAYBACK` by default. `STORED` prepares the
complete linked Wave Data spans for client-side Sample draft preview; the same
selection validation and content limits apply. It does not alter stored Samples.

Sample object details may include an `editing` snapshot for the
`a-series/sample` profile: revision, payload digest, placement, decoded
parameters, source frame bounds, and field restrictions. Its read-only
`eqCoefficients` array contains five signed Q13 values in stored order
`b1, b2, b0, -a1, -a2`; the desktop can show these in an optional response overlay.
The solid editing curve uses the unquantized parameter response. The read-only
`unavailableParameters` map uses dotted field paths and supplies a `reason`
and `message` for each omitted decoded value. `UNSUPPORTED_VALUE` identifies a
stored value outside the supported domain. `parameterCapabilities` supplies its
raw value and the active format's allowed domain so an explicit valid replacement
can repair it. Other edits preserve it. `FORMAT_UNAVAILABLE` identifies fields
not stored in this format; they are not synthesized into editable values.
`sampleFormat` identifies the stored 188/224-byte format separately from parameter
warnings and A5000 output requirements. It is also present on collection items
and object details (null for objects without an A-series Sample format).
Its HTTP `format` enum is `A3000_188`, `A4000_A5000_224`, or `UNKNOWN`;
conversion preview `targetFormat` uses the two recognized values. The embedded
alteration manifest retains its own lowercase `target_format` values.
`formatConversions` gives read-only target previews with changes and blockers;
`canConvertFormat` indicates whether the image supports the operation. Execution
uses `convert_sbnk_format`, the original payload digest and the current image
revision, and recomputes the same conversion checks. Ordinary Save never changes
format. Stereo restrictions remain in `blockedParameters`; each has an
explanation in `blockedParameterReasons` and retains its decoded value. These
metadata fields are not mutation inputs.
Unsupported layouts
return no editor profile. The desktop retains session-only drafts across the
six Sample editing tabs and object selection. Save applies only the selected
Sample's changed values, after rechecking its identity; Discard reloads that
Sample. Undo/redo applies to the unsaved draft and resets after Save.
Image close/replacement and desktop exit request confirmation for unsaved drafts.
Unconfirmed writes retain their job identity for status recovery; a refresh
failure after a confirmed write never resubmits it.

Draft audition previews playback bounds, loops, pitch, level and pan through
the desktop audio engine. It is not a hardware synthesis emulator: filters,
envelopes, LFO, routing and effects remain sampler-playback parameters. Random
pan previews at center. Destructive PCM operations and A3000 editing are not
part of this editor.

Until the first supported public release, the checked-in contract is corrected
in place and every in-repository consumer is updated with it. Compatibility
baselines and deprecation policy begin only after a contract has shipped.

## Sidecar Shutdown

Connection-file mode is intended for a desktop-owned child process. In this
mode only, an authenticated `POST /api/v1/system/shutdown` requests a clean
Crow event-loop shutdown. The process removes its connection file on exit.
Headless and LAN deployments return `404` for this endpoint and remain under
their operator's process supervisor.
