# Security

Treat disk images, object files, manifests, and audio imports as untrusted input.
The library bounds reads, validates arithmetic, rejects path traversal and unsafe
names, and uses temporary files for destination replacement.

Applications should:

- set reasonable object and output limits for their workflow;
- provide cancellation for long operations;
- write into an application-owned directory;
- reject unexpected schema major versions;
- retain source images before applying changes;
- distribute the dependency license texts and SPDX document from the SDK.

Security reports should include the axklib version, platform, input format,
stable error code, and the smallest shareable reproducer. Do not include private
sample libraries unless their redistribution is permitted.
