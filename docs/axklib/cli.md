# CLI Reference

The native CLI uses CLI11 and returns stable process categories:

| Exit | Meaning |
| ---: | --- |
| 0 | Operation completed |
| 2 | Arguments or requested operation are invalid |
| 3 | Input was read, but one or more items reported diagnostics |

Primary command groups are `info`, `inventory`, `objects`, `relationships`,
`coverage`, `validate`, `orphans`, `extract`, `create`, `alter`, and `package`.
The `package` group exports, inspects, fully verifies, plans imports, and imports
[portable object packages](portable-packages.md). Planning is read-only;
package import always fully verifies its inputs and publishes a separate image.

Structured reports are written to an output directory. Report directories
include schema metadata so consumers can reject incompatible major versions.
Normal messages lead with partition, volume, program, bank, waveform, and path
names; raw identifiers remain available in structured output.

The CLI never starts a scripting interpreter and does not depend on a Python
installation.
