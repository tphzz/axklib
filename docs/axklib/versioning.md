# Versioning And Build Identity

axklib keeps release compatibility and source traceability as separate values.
A branch name or commit hash does not replace the semantic version used by the
SDK and package metadata.

## Version Domains

| Domain | Example | Purpose |
| --- | --- | --- |
| Semantic version | `0.1.0` | C++ API compatibility, CMake package metadata, vcpkg, and SBOM package version |
| Source identity | `main-a1b2c3d` | Exact Git ref, abbreviated commit, and source-tree state compiled into a binary |
| Package basename | `axklib-main-a1b2c3d` | Development-build archive and artifact base |
| Release artifact label | `axklib-0.1.0-linux-x64` | Concise download name for the validated tag `v0.1.0` |

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

`axklib --version` reports both domains:

```text
axklib main-a1b2c3d
version: 0.1.0
package: axklib-main-a1b2c3d
git: a1b2c3d
ref: main
source: clean
```

Other CLI commands do not receive this header, so JSON, CSV, and normal command
output contracts are unchanged.

The installed SDK exposes the semantic version through `sdk_version()` and the
source identity through `sdk_build_info()`. `build_info` contains
`source_identity`, `package_basename`, `git_tag`, `git_branch`,
`git_sha_short`, `is_tagged_release`, and `is_dirty`. It is returned by value;
its string pointers refer to literals in the loaded library and remain valid
for the process lifetime.

`is_tagged_release` identifies how the source ref was selected. It does not by
itself assert that a tag is a supported axklib release version.

## Build And Artifact Names

CMake writes `package_basename.txt` to the build root and refreshes it during
each normal build. A Git-state change updates compiled metadata; an unchanged
state does not rewrite the generated C++ source.

Manual Native CI builds use these names:

| Build | Example archive stem |
| --- | --- |
| Branch Release | `axklib-main-a1b2c3d-linux-x64` |
| Branch Debug | `axklib-main-a1b2c3d-linux-x64-debug` |
| Tagged Release | `axklib-0.1.0-linux-x64` |

A workflow tag must be exactly `v<semantic-version>`. The shorter tagged
artifact name does not remove source traceability from the executable or shared
library: their embedded identity still contains the tag and commit.

After every successful Release-configuration Native CI run, the workflow
collects the Linux x64, Linux ARM64, Windows x64, Windows ARM64, and universal
macOS distributions into an unpublished GitHub draft release. Branch
`features/packages` targets the prerelease draft and generated tag
`features/packages-preview`; a tag build targets an unpublished release with
the exact version tag. A matching draft is replaced by the next successful
run. A matching published release is never deleted automatically and
causes the workflow to fail. Debug builds remain available only as workflow
artifacts.

Direct CPack filenames use the source identity captured at CMake configure
time. Reconfigure before running CPack after switching branches, tags, or
commits. Native CI reads the refreshed metadata after compilation and verifies
the staged CLI before producing its archive.
