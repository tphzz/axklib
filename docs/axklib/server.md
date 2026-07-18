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
check. Removing or relocating a workspace while an image session or active job
uses it returns a conflict. Closing the image or waiting for the job to finish
releases that workspace.

Temporary uploads are only for browser-selected audio, portable package, and
JSON manifest files. A client creates an upload, streams bounded chunks, and
completes it before using its `UploadRef`. An operation can consume an upload
only where its request schema explicitly permits one. Source disk images use a
server `FileRef`, not an upload.

WAV, SFZ, report, package, and image outputs are written to caller-selected
server `FileRef` or `DirectoryRef` destinations. They remain after the job
record expires. `GET /api/v1/files/content` provides an authenticated streamed
download, including one bounded byte range, when a user explicitly wants a
server file on the client machine.

For an explicit directory download, `POST /api/v1/files/archive` accepts a
`DirectoryRef` and creates a bounded, owner-scoped TAR snapshot in temporary
server storage. The response contains its authenticated content path and short
expiry. Download the archive, then delete that content resource; expiry and
startup cleanup are fallbacks. Archive creation rejects links, non-regular
entries, source changes, excessive entry counts, and byte-quota overflow. It
does not move, modify, or take ownership of the source directory or any durable
job output.

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
mode deliberately ignores headless config-file and environment settings so a
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
`AXKLIB_SERVER_STATE_DIRECTORY`, `AXKLIB_SERVER_CONNECTION_FILE`,
`AXKLIB_SERVER_WORKERS`, `AXKLIB_SERVER_JOB_WORKERS`,
`AXKLIB_SERVER_WRITE_JOB_WORKERS`, and
`AXKLIB_SERVER_MAX_QUEUED_JOBS`. Prefer the configuration file for the
workspace-store override, origins, token hashes, and detailed resource limits.

Non-loopback startup rejects plaintext tokens, wildcard or missing origins,
and missing named token hashes. Terminate HTTPS at a trusted reverse proxy;
Crow TLS is intentionally not enabled in axklib-server.

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
queue, image-session, and page limits. Clients should honor those values rather
than assuming compiled defaults.

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
reports configuration, sandbox, writable workspace, state-storage cleanup, and
executor-admission checks separately and returns `503` when any required check
is unavailable. Metrics are bounded aggregate request counters; they contain no
request or payload data.

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

Compatible v1 releases may add routes, schemas, enum values, response statuses,
and optional object fields. They do not remove routes, statuses, media types,
fields, or enum values; change field types; make optional request data required;
or tighten accepted numeric and size ranges. The checked-in compatibility
baseline is enforced against the generated OpenAPI document during native
tests.

### Deprecation Policy

Deprecation does not change route behavior. A deprecated resource remains
available for at least 180 calendar days after notice, and a v1 resource is not
removed from v1. Removal requires a new API major version and published
migration guidance.

Responses from a deprecated resource include all of these headers:

- `Deprecation`, using the structured date defined by
  [RFC 9745](https://datatracker.ietf.org/doc/rfc9745/);
- `Sunset`, using the HTTP date defined by
  [RFC 8594](https://datatracker.ietf.org/doc/rfc8594/); and
- `Link`, with `rel="deprecation"`, pointing to migration guidance.

The OpenAPI operation is also marked `deprecated: true`. The contract's
`x-axklib-deprecation-policy` extension exposes the minimum notice period and
required headers to generated clients.

## Sidecar Shutdown

Connection-file mode is intended for a desktop-owned child process. In this
mode only, an authenticated `POST /api/v1/system/shutdown` requests a clean
Crow event-loop shutdown. The process removes its connection file on exit.
Headless and LAN deployments return `404` for this endpoint and remain under
their operator's process supervisor.
