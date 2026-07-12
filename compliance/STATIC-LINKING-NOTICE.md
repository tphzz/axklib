# Static-linking notice

Binary builds of axklib may statically include libsndfile and libsoxr. Those
libraries are distributed under LGPL-2.1-or-later terms. Their copyright and
license notices are included in each package under `share/licenses`.

The compliance kit identifies the exact upstream source archives by SHA-512 and
includes the pinned vcpkg port recipes and patches used to build them. Axklib and
axkdeck source code, build files, pinned dependency manifest, and rebuild
instructions are the machine-readable application material for replacing those
libraries and relinking a deliverable.

This repository records a technical compliance mechanism, not legal advice.
Binary publication remains blocked until the decision record is approved by the
responsible maintainer or legal reviewer.
