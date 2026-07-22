# Application service boundary

The authoritative network contract is axklib's OpenAPI 3.1 document at
`apps/server/contracts/openapi-v1.json`. Axkdeck checks generated TypeScript
types in `src/lib/generated/axklibApiV1.ts` against that document with
`corepack pnpm contract:check`. Native handles, CXX pointer types, and axklib
internal objects are not part of this boundary.

The contract separates immediate requests, paged reads, and long-running jobs.
Jobs have monotonic progress within a phase, immutable terminal states, and
idempotent cancellation. Errors use a stable code/message/context envelope.
Large audio is represented as a file or ranged resource transfer rather than an
unbounded JSON payload.

Audition preparation returns a descriptor and a bounded WAV resource. The
client may assemble that resource with sequential range requests that respect
the server-advertised maximum range size. It decodes the complete WAV before
scheduling playback; the audio render path does not request network blocks.
This is a client playback policy and does not add a streaming transport or a
second audio API to the service contract.

`AxklibHttpApiClient` discovers the server operation registry and dispatches by
operation ID. Shared routes add an `operationId` discriminator only when the
registry advertises multiple variants. Adding an ordinary registry operation
does not add a hand-written route switch to axkdeck.

Desktop mode launches `axklib-server` on loopback port zero and authenticates
with the bearer token from its consumed connection file. Remote mode requires
HTTPS except for explicit loopback endpoints and stores credentials in the
system credential manager. WebSocket tickets are acquired through authenticated
REST; reconnect reconciles replayed events and the current job resource before
reporting completion.

The desktop shell owns the local child process. Normal shutdown uses the
authenticated server endpoint; the child also monitors the shell process ID so
it terminates after an abnormal shell exit. The shell captures the child's
stdout and stderr in a dedicated rotating platform log without exposing the
sidecar bearer token.

Persistent images and outputs are always identified by sandbox `FileRef` and
`DirectoryRef` values. A client filesystem path is never sent to a remote
process. Audio, manifest, and portable-package drag-and-drop inputs use temporary
`UploadRef` values. Explicit downloads stream an existing file or a bounded
temporary directory archive; job expiry does not delete completed sandbox
outputs.

The server owns and persists the named workspace catalogue. It may start with
no available workspace, in which case domain file operations remain unavailable
until the authenticated operator adds one. Tauri uses a native folder picker for
the local sidecar and keeps the selected absolute path in Rust until it is
committed; a remote client browses directories on the remote server. Normal
frontend state contains only workspace IDs and relative paths.

`HttpImageTransport` is the only production domain transport. The in-memory UI
test transport implements the same interface for deterministic component tests.
The Tauri shell exposes only sidecar lifecycle and protected remote-connection
settings; it does not parse, extract, create, alter, or package sampler images.
Development builds expose `F12` developer tools. Release builds retain rotating
desktop and sidecar logs but do not expose that shortcut.
