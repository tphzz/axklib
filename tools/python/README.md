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
`sourceKind` selects `FILE` (the default) or `DIRECTORY`. `additionalSources`
admits explicit sibling members in the initial source set. Optional
`companionSteps` list sibling names with `beforeNextIndex`, `status`, and
`nextRequiredIndex` assertions for incremental attachment. Directory profiles
expose admitted Files entries; `objectCount` and typed `objects` expectations
can verify later-member contents. Native reads use document-scoped resources.
Directory source files are fingerprinted
before and after the read-only run within the directory profile's bounds.
Exit codes are 0 for complete success, 1 for failures and 2 for missing coverage.
It is read-only and does not establish release readiness.

`device_capability_smoke.py` starts an isolated, owned local sidecar with device
support enabled, checks only version and operation-registry metadata, and shuts
it down. It never enumerates, opens, authorizes or recovers a device. Use
`--server`, `--output-directory`, and, on macOS, `--architecture x86_64|arm64`
to check each architecture of a packaged universal server. Logs and `result.json`
include the build identity and outcome, not credentials. Native device-policy
tests and separate manual hardware acceptance remain required.

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
`snapshot/<exact-branch-name>` and exact version-tag draft release targets.
Release validation requires five CLI archives and seven axkdeck installers for
one build identity, checks archive integrity and installer container headers,
and generates `SHA256SUMS` for those twelve packages without including itself.

`prepare_release.py` is the only GitHub release writer, invoked by the final
job of **00 Full - All Platforms**. It uses `gh api` with the release step's
`GH_TOKEN` from `secrets.GH_TOKEN`; neither the shared platform helpers nor
standalone builds receive this secret. New tag releases are drafts; existing
tag releases are preserved. Branch drafts keep their ID and full built commit
target while uploads, promotion and obsolete-file cleanup run under a durable
incomplete marker. Final inventory and SHA-256 verification must succeed before
the marker becomes complete. Lookup is paginated and errors never mean absence.
No Git refs are written and no release is published automatically.

The CLI takes `--repository`, `--ref-type`, `--ref-name`, `--commit`,
`--version-metadata-file`, `--artifacts-directory`, `--notes-file`, `--run-id`
and `--run-attempt`. Its JSON result contains `status`, `release_id`,
`tag_name`, `target_commit` and `html_url`; `--github-output` and
`--github-step-summary` write the corresponding workflow report. Artifacts
must be downloaded into separate `release-*` subdirectories in a fresh staging
root so duplicate filenames cannot be silently overwritten. The helper checks
the checkout matches the supplied full commit before release preparation.

See [release token permissions, interruption recovery and manual preview
migration](../../docs/axklib/versioning.md#release-token). A failed branch refresh
is repaired by retry; an incomplete initial tag draft is preserved on rerun
and may require ownership-checked deletion of only that draft by release ID.

`native_configure.py` runs CI CMake configuration and permits one narrowly
scoped retry when vcpkg reports a stale auxiliary-tool extraction directory.
It preserves each failed manifest while leaving package archives and installed
dependencies intact.
