# C++ API

The installed public declarations are `axklib/sdk.hpp` and its supporting
headers under `axklib/sdk/`. They compile as C++17 and use namespace `axk`.
`axklib/sdk/version.hpp` supplies generated compile-time version constants;
`sdk_version()` reports the linked runtime version.
Operations that can fail return axklib-owned `axk::result<T>` or
`axk::result<void>` values with stable error categories and native error codes.

| Type | Main responsibility |
| --- | --- |
| `operation_context` | Cancellation and optional progress reporting |
| `image` | Open media, inventory content, validate, preview, and export |
| `snapshot` | Immutable paged inventory independent of the source handle |
| `build_plan` | Validate and apply a fresh-HDS manifest |
| `transaction` | Plan and apply an ordered alteration manifest |
| `error` / `result<T>` | Owned failure and success values |

Readers use bounded random-access I/O and accept a cancellation token. Immutable
snapshots may be read concurrently. Mutation plans are single-owner values and
must be applied in their declared order.

`image` owns an immutable opened-media state. `make_snapshot()` shares that
immutable state, so the snapshot remains valid after the `image` facade is
moved or destroyed. Paged content, object, relationship, and validation-issue
results own their strings and vectors; they do not borrow storage from a
session. PCM results are owned byte vectors. Page limits must be nonzero and
callers should use `total_count` to bound subsequent requests.

`operation_context` owns cancellation state and may be cancelled from another
thread. A progress sink is borrowed: it must remain alive until the operation
returns or until it is detached with `set_progress_sink(nullptr)`. Reporting is
synchronous, callbacks must return promptly, and exceptions thrown by a callback
are contained and ignored. A callback must not destroy or move the context that
is invoking it and must not call `set_progress_sink`. Detaching waits for an
in-flight callback to return, after which the former sink may be destroyed.
Snapshot reads are concurrent; `image`, `build_plan`,
`transaction`, and context mutation require external serialization. Build plans
and transactions are bound to their creating thread when applied.

Facade classes use PIMPL storage. Raw record, parser, allocation, codec, JSON,
and transport types are not installed. Exceptions from the implementation are
contained and converted to `error`; `result::value()` itself follows the normal
expected-style precondition and throws `bad_result_access` when misused.
Moving a successful result transfers its owned value. Calling `value()` on a
failure or `error()` on a success throws `bad_result_access`; inspect the result
with `operator bool()` first. Public operations contain allocation, filesystem,
codec, and implementation exceptions and return an `internal` error instead.

The pre-1.0 binary contract is compiler-family and runtime specific. Public
facade objects contain only PIMPL ownership, but C++ standard-library values are
part of signatures. Build consumers with a supported ABI-compatible C++17
runtime for the downloaded SDK package.
