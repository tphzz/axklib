# Python Development Tools

This project contains repository and release utilities that are independent of
the axklib implementation. It is not an axklib language binding and is not
required to configure, build, install, or use the native SDK or CLI.

`image_open_smoke.py` accepts a small external corpus manifest and checks image
opening through an isolated server API, including completed job serialization,
Files enumeration and supported Device navigation. Run it with `--server`,
`--manifest`, repeatable `--root NAME=PATH`, and a new `--output` directory.
Each manifest case names `id`, `root`, relative `path` and `expected`
(`format`, `filesystemName`, `deviceView`, `minimumFiles`; optionally `names`
and `metadata`). A null path requires `unavailableReason` and reports a gap.
Exit codes are 0 for complete success, 1 for failures and 2 for missing coverage.
It is read-only and does not establish release readiness.

`image_open_extended.py` uses the same opening/navigation checks for a separate
manifest of up to 256 cases, without enlarging the default 16-case smoke bound.
Its Linux supervisor requires `prlimit` and `timeout`: each sequential worker
has a 4 GiB address-space ceiling, disabled core dumps and a 120-second timeout.
It records per-worker peak RSS through `wait4`, elapsed time, exit status and
logs. Enumeration allows 90 seconds, 8,192 requests and 250,000 entries per view;
the suite deadline is 30 minutes. Use the same CLI arguments as the mini scan.
Missing images and failed cases remain explicit; no baseline is rewritten.
The extended diagnostic also enumerates invalid-but-openable images and retains
their paginated validation issues. It still reports these as failed cases,
separating validation health from completed Files/Device navigation. The default
mini smoke continues to reject validation errors immediately.

Run its quality gates from the repository root:

```bash
uv --project tools/python run ruff check tools/python
uv --project tools/python run mypy tools/python
uv --project tools/python run pytest tools/python/tests
```

The header generators consume versioned JSON tables under `library/data` and
write deterministic C++ headers. Package inspection, SBOM generation, boundary
checking, and benchmark comparison are used by release workflows.

`axk-release-metadata` validates generated Git source metadata, resolves the
source-derived archive name for a platform and build configuration, and checks
that a staged native CLI reports the expected commit and ref. It also resolves
branch-preview and version-tag draft release targets and verifies the complete
release asset set against its SHA-256 checksum files.

`native_configure.py` runs CI CMake configuration and permits one narrowly
scoped retry when vcpkg reports a stale auxiliary-tool extraction directory.
It preserves each failed manifest while leaving package archives and installed
dependencies intact.
