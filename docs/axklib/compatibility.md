# Compatibility

axklib versions three public surfaces independently:

- The C++ API follows the project semantic version.
- The C ABI exposes `AXK_ABI_VERSION_MAJOR` and size-versioned structures.
- JSON and CSV schemas carry their own major and minor versions.

The C ABI major remains load-compatible across compatible releases. New fields
are appended to structures and guarded by `struct_size`. A caller compiled
against an older v1 header can use a newer v1 shared library.

CLI commands retain option names, structured field meanings, and exit categories
within a major release. Human-readable formatting may gain additional context.

The SDK does not ship a supported Python binding. Applications should use the
C++ target, stable C ABI, native CLI, or a host-language binding built over the
C ABI.
