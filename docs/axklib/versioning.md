# Versioning And Build Identity

The monorepo derives one axklib/axkdeck product version from Git release tags.
Source identity is a separate value that identifies the exact checkout used for
a build.

## Release Version

An official release is built from a detached commit with exactly one valid
semantic-version tag. The tag may use either form:

```text
[v]MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

Examples include `1.2.3`, `v1.2.3`, `1.2.3-rc.1`, and
`v1.2.3-rc.1+build.4`. An optional leading `v` belongs to the Git tag and is
omitted from runtime, SDK, package, and SBOM semantic-version strings. The exact
tag spelling is retained as release metadata and as the GitHub release tag.

The complete semantic version is used by:

- `axklib --version` and the no-argument CLI banner;
- `axk::version()` and `axk::sdk_version()`;
- installed SDK version constants;
- release artifact names and SPDX SBOM metadata; and
- CPack package metadata;
- axkdeck application and installer metadata; and
- the identity axkdeck requires from its bundled local `axklib-server`.

CMake's `project(VERSION)` field and shared-library ABI filenames require a
numeric version. They use only the `MAJOR.MINOR.PATCH` core. For example,
`v1.2.3-rc.1+build.4` has semantic version `1.2.3-rc.1+build.4` and CMake
project version `1.2.3`.

Recognized version branches provide prerelease identity before a release tag
exists. These branch names are accepted:

```text
MAJOR.MINOR.PATCH
vMAJOR.MINOR.PATCH
release/MAJOR.MINOR.PATCH
release/vMAJOR.MINOR.PATCH
feature/MAJOR.MINOR.PATCH
feature/vMAJOR.MINOR.PATCH
features/MAJOR.MINOR.PATCH
features/vMAJOR.MINOR.PATCH
bugfix/MAJOR.MINOR.PATCH
bugfix/vMAJOR.MINOR.PATCH
bugfixes/MAJOR.MINOR.PATCH
bugfixes/vMAJOR.MINOR.PATCH
```

They require a strict numeric semantic-version core with no prerelease or build
suffix. The namespace, when present, must be exactly one of the five listed
names; arbitrary namespaces and nested paths are ordinary development branches.
A `0.3.6`, `release/v0.3.6`, or `features/0.3.6` branch produces semantic
version `0.3.6-pre`, numeric project version `0.3.6`, and clean source identity
`0.3.6-pre-<short-git-object-id>`. Version-branch builds are prereleases, not
releases, and have no release tag.

Other named branches and source trees without usable Git metadata deliberately
use product version `0.0.0`. A release version is never guessed from a manifest
or tracked literal. The top-level vcpkg manifest therefore does not declare an
independent axklib version. Axkdeck's `package.json` and `Cargo.toml` use the
non-authoritative `0.0.0` development placeholder, while `tauri.conf.json`
deliberately has no tracked version. The desktop build wrapper injects the
generated native version into Tauri at build time.

In GitHub Actions, the selected workflow ref type is explicit: a branch run is
always a non-release build, while a tag run validates `GITHUB_REF_NAME` against
`HEAD`. This avoids making version selection depend on how checkout represents
the ref locally.

Configuration fails when a GitHub tag build uses an invalid tag, when the
selected tag does not identify `HEAD`, or when a detached local commit has more
than one valid semantic-version tag. Switching a configured tree between a
development checkout and a release tag requires rerunning CMake.

## Source Identity

Clean ordinary branches and release tags use:

```text
<sanitized-ref>-<short-git-object-id>
```

Dirty ordinary branches and release tags place `dirty` before the Git object ID:

```text
<sanitized-ref>-dirty-<short-git-object-id>
```

Recognized version branches normalize every accepted spelling to one of:

```text
MAJOR.MINOR.PATCH-pre-<short-git-object-id>
MAJOR.MINOR.PATCH-pre-dirty-<short-git-object-id>
```

`dirty` means the build observed staged, unstaged, deleted, conflicted, or
untracked non-ignored files. The Git object ID is always the final identity
token. Git object abbreviations contain at least seven hexadecimal characters
and may be longer when Git needs more characters for uniqueness.

The selected ref uses this order:

1. An explicit GitHub Actions branch or tag ref.
2. A checked-out named branch.
3. An exact semantic-version tag at detached `HEAD`.
4. `detached`.

Ref names retain ASCII letters, digits, dots, underscores, and hyphens. Other
character runs become one hyphen. A ref with no remaining characters becomes
`unknown`.

When Git is unavailable, local builds use `local-unknown`. When Git is present
but the source directory has no usable repository or `HEAD`, they use
`detached-unknown`. These fallbacks let source archives build without a runtime
or mandatory configure-time Git dependency. Official CI packages require a
known commit and a clean source tree.

## Runtime Information

For a branch build, `axklib --version` reports:

```text
axklib main-a1b2c3d
version: 0.0.0
package: axklib-main-a1b2c3d
git: a1b2c3d
ref: main
source: clean
```

For any recognized clean `0.3.6` version branch, the corresponding fields are:

```text
axklib 0.3.6-pre-a1b2c3d
version: 0.3.6-pre
package: axklib-0.3.6-pre-a1b2c3d
git: a1b2c3d
ref: <sanitized branch name>
source: clean
```

For a detached `v1.2.3` release build, `version` is `1.2.3` and the ref is
`v1.2.3`. Other CLI commands do not receive this header, so JSON, CSV, and
normal command output contracts are unchanged.

The installed SDK exposes the product version through `sdk_version()` and the
source identity through `sdk_build_info()`. `build_info` contains
`source_identity`, `package_basename`, `git_tag`, `git_branch`,
`git_sha_short`, `is_tagged_release`, and `is_dirty`. It is returned by value;
its string pointers refer to literals in the loaded library and remain valid
for the process lifetime.

## Workflow Names

The Actions sidebar groups workflows by role:

| Workflow | Purpose | File in `.github/workflows/` |
| --- | --- | --- |
| **00 Full - All Platforms** | All targets; Release builds publish a draft | `native.yml` |
| **10 Build - Windows x64** | One platform's test artifacts | `native-windows-x64.yml` |
| **11 Build - Windows ARM64** | One platform's test artifacts | `native-windows-arm64.yml` |
| **12 Build - macOS Universal** | Universal macOS test artifacts | `native-macos.yml` |
| **13 Build - Linux x64** | One platform's test artifacts | `native-linux-x64.yml` |
| **14 Build - Linux ARM64** | One platform's test artifacts | `native-linux-arm64.yml` |
| **30 Deploy - Docs** | Documentation deployment | `publish-docs.yml` |
| **90 Helper - Checks** | Shared prerequisite checks | `native-checks.yml` |
| **91 Helper - Linux/Windows** | Shared native build recipe | `native-platform.yml` |
| **92 Helper - macOS Universal** | Shared macOS build recipe | `native-macos-platform.yml` |

The numbers group navigation; they do not specify execution order. Start a
**Full** or **Build** workflow, not a **Helper**. Helpers accept only
`workflow_call`, so they have no **Run workflow** button. GitHub's generated
`pages-build-deployment` entry is outside this naming scheme.

The six native entry points use run titles such as
`10 Build - Windows x64 | v0.6.0 | Release`. The title reflects the selected
source branch and Debug/Release mode. GitHub documents these display fields as
[`name` and `run-name`](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#run-name).

Keep display metadata consistent on `main` and the branch being built. If their
build logic differs, synchronize only top-level `name` and `run-name` fields.
Keep workflow filenames, job IDs and reusable-workflow paths stable. Commands
can continue to use filenames, for example:

```bash
gh workflow run native-windows-x64.yml --ref v0.6.0
```

The naming policy is checked by `tools/python/tests/test_workflow_names.py`.

## Build And Artifact Names

CMake writes `version_metadata.json` and `package_basename.txt` to the build
root. Release and SBOM tools consume these generated files rather than parsing
separate version declarations. A Git-state change refreshes compiled source
identity metadata; an unchanged state does not rewrite generated files.

Manual native builds use these artifact names:

| Build | Example archive stem |
| --- | --- |
| Branch CLI Debug | `axklib-cli-main-a1b2c3d-linux-x64-debug` |
| Dirty branch CLI Debug | `axklib-cli-main-dirty-a1b2c3d-linux-x64-debug` |
| Version-branch CLI | `axklib-cli-0.3.6-pre-a1b2c3d-linux-x64` |
| Tagged CLI prerelease | `axklib-cli-1.2.3-rc.1-linux-x64` |
| Branch axkdeck DEB | `axkdeck-main-a1b2c3d-linux-x64.deb` |
| Version-branch axkdeck DEB | `axkdeck-0.3.6-pre-a1b2c3d-linux-x64.deb` |
| Tagged axkdeck NSIS installer | `axkdeck-1.2.3-windows-x64.exe` |
| Tagged axkdeck prerelease DMG | `axkdeck-1.2.3-rc.1-macos-universal.dmg` |

Ordinary branch desktop binaries carry semantic version `0.0.0`. Version-branch
desktop binaries carry `MAJOR.MINOR.PATCH-pre`. Both carry the same source
identity as their bundled server. Tagged desktop binaries carry the complete
semantic version. The axkdeck About dialog shows only the semantic version for
a tagged release and the source identity for a non-release build. On macOS,
`CFBundleShortVersionString` uses the numeric
`MAJOR.MINOR.PATCH` core and `CFBundleVersion` uses a numeric CI build number,
while the complete semantic version and source identity remain embedded in the
application runtime metadata.

After every successful Release run of **00 Full - All Platforms**, the workflow
collects CLI archives for Linux x64, Linux ARM64, Windows x64, Windows ARM64,
and universal macOS into an unpublished GitHub draft release. The same draft
contains axkdeck DEB and RPM installers for both Linux
architectures, NSIS installers for both Windows architectures, and one universal
macOS DMG. `axklib-server` is shipped in those desktop installers rather than as
a standalone release asset. The C++ library is distributed as source rather
than as a prebuilt SDK archive. The Windows installers are currently unsigned;
the full-platform workflow does not accept or retain exportable PFX signing
credentials. Before a release write, the helper validates all twelve packages,
their shared build identity, archive contents and installer container headers.
Native installer execution, dependency and signing checks remain in the
platform jobs. It generates `SHA256SUMS` with one entry per package, excluding
the checksum file itself. A complete newly prepared draft has thirteen
assets; no release-manifest sidecar is uploaded.

A branch uses one current prerelease draft with pending tag
`snapshot/<exact-branch-name>` and the full built commit as its target.
For example, branch `v0.6.0` uses `snapshot/v0.6.0`, while package filenames
remain `axklib-cli-0.6.0-pre-<short-id>-<platform>.<extension>` and
`axkdeck-0.6.0-pre-<short-id>-<platform>.<extension>`. Slashes and other branch
characters remain unchanged in the pending tag; filename sanitization is
separate. A Git tag build uses that exact tag spelling and its complete
semantic version. Semantic-version prereleases are marked as GitHub
prereleases. An existing release for that tag, whether draft or published,
is preserved without changing its assets, notes or metadata. In particular,
older published releases do not acquire a checksum file automatically.

Branch snapshots and tag releases have different retention policies. A new
branch build prepares a separate draft and replaces the previous branch draft
only after the new draft is complete and verified. Existing tag releases are
never replaced or refreshed, whether unpublished drafts or published releases.
A draft associated with an existing Git tag remains a tag release even if it
originated as a branch snapshot and still carries branch preparation metadata.

Preparation neither publishes a release nor creates, moves or deletes Git
refs. A snapshot name is a pending release tag, not a generated Git tag.
Published snapshots, conflicting snapshot Git refs, duplicate matching
releases and unmigrated branch previews stop preparation for explicit manual
reconciliation. Authentication and network errors are failures; they do not mean
that a release is absent. Release and asset lookup includes every API page.
The workflow reports `created`, `replaced` or `preserved` and the actual
GitHub release URL. Debug builds remain available only as workflow artifacts.

### Release Token

The repository secret `GH_TOKEN` is required only on the release preparation
step, as `GH_TOKEN: ${{ secrets.GH_TOKEN }}`. It must allow repository Contents
write access. When the built commit differs from the default branch in
`.github/workflows/`, GitHub also requires Workflows write access (or the
`workflow` scope for a classic token). This applies to the current `v0.6.0`
branch. The job's built-in token remains Contents read and Actions read;
workflow `permissions:` does not grant privileges to the secret. The helper
does not fall back to `github.token` or `GITHUB_TOKEN`, print token values or
manage secrets. See [GitHub's release permissions](https://docs.github.com/en/rest/releases/releases#create-a-release).

### Interrupted Preparation

Branch replacements use a separate incoming draft with a unique
`axklib-incoming/<operation>` pending tag and the full built commit as target.
Human notes outside the previous draft's managed section are copied into the
candidate. The previous branch draft, including its metadata and assets, stays
unchanged while the candidate is marked **INCOMPLETE** and uploaded. Incoming
files use temporary names incorporating their digest; the checksum is promoted
last. Every final filename, uploaded state, size and SHA-256 digest must match
before the candidate is marked **COMPLETE**. Its final inventory contains exactly
the twelve packages and `SHA256SUMS`.

Only after that complete inventory is reverified does the helper delete the
previous branch draft and assign the canonical `snapshot/<exact-branch-name>`
pending tag to the candidate. The new current snapshot has a new release ID.
Neither operation creates or changes an actual Git ref.

If preparation fails or is cancelled before cutover, the previous snapshot
remains intact. Retry the full workflow to resume the incoming candidate's
uploads, verification and cutover, even when the commit is unchanged. If deletion
succeeded but assigning the pending tag failed, the complete incoming draft still
retains all new packages; retry resumes from it. Multiple incoming candidates,
changed incoming metadata, a published canonical snapshot or conflicting Git
ref require manual reconciliation. Preparation never reverts publication or
changes a ref. Incoming ownership markers are checked even when an operator has
renamed the pending tag outside the incoming namespace.
Incoming notes carry a fingerprint so operator edits made between attempts are
detected before retry writes; title and prerelease flags are checked as well.
Before each branch-draft mutation, the helper rechecks both the candidate's
pending tag and the canonical snapshot name for conflicting Git refs. A tag
created during preparation stops subsequent mutations; legitimate first-time
preparation of a new version-tag draft remains supported. These checks and the
workflow lock do not make separate GitHub API requests atomic with manual edits.

If initial tag preparation fails, cleanup may remove only the draft created
by that invocation, verified by ID and its managed ownership marker. The Git
tag is retained. A cancellation or failed cleanup may leave an incomplete
draft. An ordinary tag rerun preserves that draft, so manual recovery is
required:

1. Use the failure summary's release URL and numeric ID. Inspect the release
   and its managed `operation`, run, ref and commit fields; verify it is still
   the failed invocation's unpublished draft.
2. If that verification succeeds, delete only that release object by ID.
   Never delete a pre-existing or published release, and retain its Git tag.
3. Retry the full workflow for the original Git tag.

For example, replace `NEW_DRAFT_ID` with the verified numeric ID:

```bash
gh api repos/tphzz/axklib/releases/NEW_DRAFT_ID
# Only after verifying ownership and draft=true:
gh api --method DELETE repos/tphzz/axklib/releases/NEW_DRAFT_ID
```

Do not use tag-cleanup flags or Git-ref deletion commands. If creation's outcome
is unknown, inspect releases for the reported pending tag and operation marker
before deciding which object, if any, belongs to the failed invocation.

### One-Time Preview Migration

The naming rollout is separate from ordinary snapshot replacement. On
2026-11-10, draft `431006702` (`v0.6.0-preview`) was explicitly approved for a
one-time rename of its pending tag and title to `snapshot/v0.6.0`. The rename
preserved its ID, twelve assets, notes, target
`8c783a71369a41768ae848830e06cc92c873fb3f` and draft/prerelease flags. No release
was published and no Git ref changed. Subsequent branch builds follow the
replacement policy above. Historical `v0.5.1-preview`, published releases and
Git tags remain unchanged; any other legacy preview requires explicit manual
reconciliation instead of an automatic naming migration.

The policy applies to refs containing these updated workflows and helpers.
Existing Git tags retain their historical workflow definitions; their direct
workflow reruns retain the old behavior. No historical-tag orchestration input
is provided, and tags must not be moved to roll out this policy.

For faster iterations, **10 Build - Windows x64**, **11 Build - Windows ARM64**,
**13 Build - Linux x64** and **14 Build - Linux ARM64** each build one target,
using the same build, test and packaging recipe as the full-platform workflow.
Each first runs the shared architecture, release-tool and desktop-static checks.
Select the source branch in **Run workflow** and leave **Debug** off for Release
artifacts. These runs upload
installers and CLI archives without creating or replacing a draft release.
**00 Full - All Platforms** remains the only draft-release preparer.
Windows device reconnection has a separate
[read-only validation checklist](../windows-device-validation.md).

The separate manual **12 Build - macOS Universal** workflow reuses the same
macOS jobs for faster platform-specific iterations. It always delivers signed
universal DMG and CLI workflow artifacts, leaves the all-platform draft release untouched,
and offers optional notarization (off by default). Device access is compiled
into both slices; packaged capability checks do not qualify physical writes.
See the [remote macOS validation checklist](../macos-device-validation.md).

The version fields in `docs/pyproject.toml` and `tools/python/pyproject.toml`
version their independent Python environments. They are not monorepo product
version sources.
