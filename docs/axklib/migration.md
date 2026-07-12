# Native Migration

The first native release replaces the former Python distribution.

| Former integration | Native replacement |
| --- | --- |
| Python package calls | `axklib::core` C++ calls |
| Python extension host | `axklib::c` opaque handles |
| Python console script | Installed `axklib` CLI11 executable |
| Python dictionaries | Versioned C structs or JSON reports |
| Python exceptions | `axk::Result<T>`, `axk_status`, and stable error codes |

Structured CLI consumers should validate the report schema major version and
continue using sampler-facing selectors. Embedders should prefer the C ABI when
compiler or standard-library ABI compatibility cannot be guaranteed.

The final Python production line is tagged `python-v0.0.0`; the native line
starts at `v0.1.0`. Downgrading the application does not rewrite images already
created or altered. Retain source images and manifests when a rollback must be
possible.
