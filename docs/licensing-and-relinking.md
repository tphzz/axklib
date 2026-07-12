# Licensing and relinking

The shared SDK, CLI, and desktop application statically include their private
native dependencies. Release packages contain the corresponding copyright and
license notices. In particular, libsndfile and libsoxr are covered by
LGPL-2.1-or-later terms.

The separate compliance artifact contains:

- SHA-512 identities for the exact libsndfile and libsoxr source archives;
- the pinned vcpkg baseline, port recipes, and applied patches;
- the public source revision used for the application material;
- instructions for replacing either LGPL library and relinking from source;
- the binary-publication approval record.

The SDK and CLI are separate binary artifacts, but they share this compliance
artifact. Axkdeck records its own public source revision and uses the same pinned
native recipes. Release automation must collect the exact upstream archives and
successfully rebuild with replacement-compatible libraries before a binary can
be published.

The technical mechanism does not constitute legal advice. The checked-in
approval record remains authoritative for whether project binaries may be
published.
