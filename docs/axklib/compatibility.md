# Compatibility

axklib versions four public surfaces independently:

- The C++ API follows the project semantic version.
- Each binary carries a Git-derived source identity for precise build identification.
- JSON and CSV schemas carry their own major and minor versions.
- Portable object packages carry an independent manifest schema version.

Installed PIMPL classes constrain public object layout, but pre-1.0 releases may
still change source or binary compatibility. Release builds derive their version
from a semantic-version Git tag; development builds use product version `0.0.0`.
Supported consumers use C++17 and a documented compiler/runtime combination.
Source identities such as `main-a1b2c3d` do not change the API compatibility
version. See [Versioning And Build Identity](versioning.md).

| Package | Build toolchain | Architectures |
| --- | --- | --- |
| Windows SDK/CLI | Visual Studio 18 MSVC, dynamic CRT | x64, ARM64 |
| Linux SDK/CLI | GCC 14, distribution libstdc++/glibc | x64, ARM64 |
| macOS SDK/CLI | Apple Clang from Xcode 16.4 | universal x64 + ARM64 |

A shared SDK package is compatible only with the matching compiler-family C++
ABI and runtime generation. The CLI has no axklib runtime dependency but still
uses the documented platform C/C++ runtime libraries.

After a public release, CLI commands retain option names, structured field
meanings, and exit categories within that major release. Human-readable
formatting may gain additional context.

The canonical object terminology is Sample (`SBNK`) and Sample Bank (`SBAC`).
Writer and alteration manifest schema `1.0`, object JSON schema `1.0`, and
`axklib.volume_graph.v1` use that terminology directly. `SampleSpec` models
`SBNK`; `SampleBankSpec` models `SBAC`; and package roots use `sample`,
`sample-bank`, and `wave-data`. Obsolete pre-release names are rejected and
have no compatibility aliases or migration readers.

Until the first supported public release, axklib corrects unreleased contracts
in place and updates all in-repository consumers together. Compatibility
versions and migration paths begin only when a released contract or real
persisted user data requires them.

The current portable-package reader accepts exactly manifest schema `1.0` and
rejects unknown or missing fields. All typed package extensions share that one
schema; the manifest, not the filename, determines the package kind. See
[Portable Object Packages](portable-packages.md) for the version and target
compatibility contract.

SFS package imports reuse waveform objects only inside the destination volume.
Hardware testing rejected cross-volume SMPL storage: the dependent volume was
listed but could not load independently. The reserved partition-wide policy is
therefore unavailable in package schema 1.0 rather than an opt-in optimization.

The SDK does not ship a C interface, Python binding, or general-purpose FFI.
Applications use the C++ target or invoke the native CLI.
