# Security

Treat disk images, object files, manifests, and audio imports as untrusted input.
The library bounds reads, validates arithmetic, rejects path traversal and unsafe
names, and uses temporary files for destination replacement.

Portable-package inspection reads only bounded ZIP metadata and
`manifest.json`; it deliberately reports that payloads are not verified. Full
verification streams every declared payload, checks its digest, decodes its
object profile, and validates graph closure and relocation bindings. Import
always performs full verification. Archive path rules and hard limits are
specified in [Portable Object Packages](portable-packages.md).

Applications should:

- set reasonable object and output limits for their workflow;
- provide cancellation for long operations;
- write into an application-owned directory;
- reject unexpected schema major versions;
- retain source images before applying changes;
- distribute the dependency license texts and SPDX document from the SDK.

Security reports should include the axklib version, platform, input format,
stable error code, and the smallest shareable reproducer. Do not include private
sample libraries unless their redistribution is permitted. Report suspected
vulnerabilities through the repository's
[private vulnerability reporting form](https://github.com/tphzz/axklib/security/advisories/new),
not a public issue. Include the affected version, impact, reproduction steps,
and whether the report may contain confidential sample data.
