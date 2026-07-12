# Native Migration

The first native release replaces the former Python distribution.

| Former integration | Native replacement |
| --- | --- |
| Earlier native headers | `axklib/sdk.hpp` C++17 facade |
| Earlier C handles | C++ PIMPL session and plan values |
| Python console script | Installed `axklib` CLI11 executable |
| Python dictionaries | C++ DTOs or versioned JSON reports |
| Python exceptions | `axk::result<T>` and stable error codes |

Structured CLI consumers should validate the report schema major version and
continue using sampler-facing selectors. Native embedders link the C++17 shared
SDK. Other integrations can use the CLI until a separately versioned service is
available.

The native SDK and CLI remain versioned together at `v0.1.0`. Downgrading the
application does not rewrite images already
created or altered. Retain source images and manifests when a rollback must be
possible.
