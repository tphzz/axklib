# Compatibility

axklib versions two public surfaces independently:

- The C++ API follows the project semantic version.
- JSON and CSV schemas carry their own major and minor versions.

The project is currently `0.1.0`. Installed PIMPL classes constrain public object
layout, but pre-1.0 releases may still change source or binary compatibility.
Supported consumers use C++17 and a documented compiler/runtime combination.

| Package | Build toolchain | Architectures |
| --- | --- | --- |
| Windows SDK/CLI | Visual Studio 18 MSVC, dynamic CRT | x64, ARM64 |
| Linux SDK/CLI | GCC 14, distribution libstdc++/glibc | x64, ARM64 |
| macOS SDK/CLI | Apple Clang from Xcode 16.4 | universal x64 + ARM64 |

A shared SDK package is compatible only with the matching compiler-family C++
ABI and runtime generation. The CLI has no axklib runtime dependency but still
uses the documented platform C/C++ runtime libraries.

CLI commands retain option names, structured field meanings, and exit categories
within a major release. Human-readable formatting may gain additional context.

The SDK does not ship a C interface, Python binding, or general-purpose FFI.
Applications use the C++ target or invoke the native CLI.
