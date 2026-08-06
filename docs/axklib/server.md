# REST And WebSocket Server

`axklib-server` exposes the maintained axklib operations to axkdeck and other
authenticated clients. It uses upstream Crow for JSON REST routes and the
server-to-client job-event WebSocket. The server does not host the axkdeck web
application and does not accept commands over WebSocket.

## Storage Model

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

Workspace configuration is stored per user at:

- `$XDG_CONFIG_HOME/axkdeck/workspaces.json`, or
  `~/.config/axkdeck/workspaces.json`, on Linux;
- `%APPDATA%\axkdeck\workspaces.json` on Windows; and
- `~/Library/Application Support/axkdeck/workspaces.json` on macOS.

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

Temporary uploads are only for browser-selected audio, portable package, and
JSON manifest files. A client creates an upload, streams bounded chunks, and
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

The application listener is plaintext because Crow TLS is intentionally not
enabled. Non-loopback startup is therefore rejected unless
`allowInsecureRemoteHttp` or `--allow-insecure-remote-http` is explicit, in
addition to named token hashes and non-wildcard origins. The flag acknowledges
that bearer credentials are exposed on the application connection; it does not
make HTTP secure. Keep the listener loopback-only or place it on a private
backend behind a trusted HTTPS reverse proxy. Never expose plaintext remote
mode directly to an untrusted network.

## Operations And Jobs

`GET /api/v1/system/capabilities` is the runtime operation catalogue. Domain
routes, execution mode, request schema, result schema, and shared-route variant
come from the same application registry used to validate the CLI command
catalogue. This keeps the Crow adapter independent of individual domain
operation IDs.

The server is intentionally a generic transport adapter. A maintained domain
operation is added to the application registry, implemented in the
transport-neutral application layer, and exposed automatically through the
registry dispatcher and generated OpenAPI document. It must not add a Crow
route or handler. `bind_application_operations(...)` is the single composition
point for stateful operation families, so the server does not know which
application modules implement them. The build includes an architecture check
that rejects hard-coded domain operation IDs, individual application binders,
alternate HTTP frameworks, and Crow includes outside `apps/server`.

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

When an explicit operation encounters missing split Wave Data,
`POST /api/v1/images/{imageId}/companion-directories` attaches either a selected
list of `DirectoryRef` values or the explicitly requested immediate siblings.
The server checks only those directories and admits exact continuation segments
with normalized Yamaha header identities, even when Yamaha changes the host
filename between disks, plus Wave Data objects whose embedded names exactly
satisfy active unresolved Sample member lanes. The image ID and object IDs
remain stable, while the session revision advances. Attachments are retained
only for the lifetime of that image session; the source folders and their files
are never merged or modified.

`POST /api/v1/files/list` performs only a bounded directory listing. A media
picker navigates directories without media inspection and inspects the current
directory with `POST /api/v1/files/media-source/inspect` only when the user
chooses to open it. The
response reports `mediaSourceKind: "AXK_OBJECT_DIRECTORY"` when bounded
file-prefix inspection recognizes Yamaha object data, otherwise `null`.
Inspection does not decode complete payloads or recurse beyond one related
disk-folder level.

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
reports that physical sampler validation remains pending.

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
on storage with enough free space for the configured upload total. Increase
workers only after measuring the actual image and extraction workload. A
single large domain operation can require substantially more memory than the
HTTP transport, so the transport budget is not a whole-image memory promise.

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

Until the first supported public release, the checked-in contract is corrected
in place and every in-repository consumer is updated with it. Compatibility
baselines and deprecation policy begin only after a contract has shipped.

## Sidecar Shutdown

Connection-file mode is intended for a desktop-owned child process. In this
mode only, an authenticated `POST /api/v1/system/shutdown` requests a clean
Crow event-loop shutdown. The process removes its connection file on exit.
Headless and LAN deployments return `404` for this endpoint and remain under
their operator's process supervisor.
