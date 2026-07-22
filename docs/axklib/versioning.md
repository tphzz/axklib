# Versioning And Build Identity

axklib derives its product version from Git release tags. Source identity is a
separate value that identifies the exact checkout used for a build.

## Release Version

An official release is built from a detached commit with exactly one valid
semantic-version tag:

```text
vMAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

Examples include `v1.2.3`, `v1.2.3-rc.1`, and
`v1.2.3-rc.1+build.4`. The leading `v` belongs to the Git tag and is omitted
from runtime, SDK, package, and SBOM version strings.

The complete semantic version is used by:

- `axklib --version` and the no-argument CLI banner;
- `axk::version()` and `axk::sdk_version()`;
- installed SDK version constants;
- release artifact names and SPDX SBOM metadata; and
- CPack package metadata.

CMake's `project(VERSION)` field and shared-library ABI filenames require a
numeric version. They use only the `MAJOR.MINOR.PATCH` core. For example,
`v1.2.3-rc.1+build.4` has semantic version `1.2.3-rc.1+build.4` and CMake
project version `1.2.3`.

Named-branch builds and source trees without usable Git metadata deliberately
use product version `0.0.0`. A release version is never guessed from a branch,
manifest, or tracked literal. The top-level vcpkg manifest therefore does not
declare an independent axklib version.

In GitHub Actions, the selected workflow ref type is explicit: a branch run is
always a development build, while a tag run validates `GITHUB_REF_NAME` against
`HEAD`. This avoids making version selection depend on how checkout represents
the ref locally.

Configuration fails when a GitHub tag build uses an invalid tag, when the
selected tag does not identify `HEAD`, or when a detached local commit has more
than one valid semantic-version tag. Switching a configured tree between a
development checkout and a release tag requires rerunning CMake.

## Source Identity

The source identity has the form:

```text
<sanitized-ref>-<short-git-object-id>[-mod]
```

`-mod` means the build observed staged, unstaged, deleted, conflicted, or
untracked non-ignored files. Git object abbreviations contain at least seven
hexadecimal characters and may be longer when Git needs more characters for
uniqueness.

The selected ref uses this order:

1. A checked-out named branch.
2. A GitHub Actions tag ref for a detached tag build.
3. An exact local tag at detached `HEAD`.
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

For a detached `v1.2.3` release build, `version` is `1.2.3` and the ref is
`v1.2.3`. Other CLI commands do not receive this header, so JSON, CSV, and
normal command output contracts are unchanged.

The installed SDK exposes the product version through `sdk_version()` and the
source identity through `sdk_build_info()`. `build_info` contains
`source_identity`, `package_basename`, `git_tag`, `git_branch`,
`git_sha_short`, `is_tagged_release`, and `is_dirty`. It is returned by value;
its string pointers refer to literals in the loaded library and remain valid
for the process lifetime.

## Build And Artifact Names

CMake writes `version_metadata.json` and `package_basename.txt` to the build
root. Release and SBOM tools consume these generated files rather than parsing
separate version declarations. A Git-state change refreshes compiled source
identity metadata; an unchanged state does not rewrite generated files.

Manual Native CI builds use these names:

| Build | Example archive stem |
| --- | --- |
| Branch SDK Release | `axklib-sdk-main-a1b2c3d-linux-x64` |
| Branch CLI Debug | `axklib-cli-main-a1b2c3d-linux-x64-debug` |
| Tagged SDK Release | `axklib-sdk-1.2.3-linux-x64` |
| Tagged CLI prerelease | `axklib-cli-1.2.3-rc.1-linux-x64` |

After every successful Release-configuration Native CI run, the workflow
collects separate SDK and CLI archives for Linux x64, Linux ARM64, Windows x64,
Windows ARM64, and universal macOS into an unpublished GitHub draft release.
The same draft contains axkdeck DEB and RPM installers for both Linux
architectures, NSIS installers for both Windows architectures, and one universal
macOS DMG. `axklib-server` is shipped in those desktop installers rather than as
a standalone release asset. GitHub displays the digest for each asset, so the
workflow does not attach checksum or release-manifest sidecars. Branch
`features/packages` targets the prerelease draft and generated tag
`features/packages-preview`; a semantic-version tag build targets an
unpublished release with that exact tag. Semantic-version prereleases are
marked as GitHub prereleases. A matching draft is replaced by the next
successful run. A matching published release is never deleted automatically
and causes the workflow to fail. Debug builds remain available only as
workflow artifacts.

The version fields in `docs/pyproject.toml` and `tools/python/pyproject.toml`
version their independent Python environments. They are not axklib product
version sources.
