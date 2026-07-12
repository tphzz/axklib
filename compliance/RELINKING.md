# Rebuilding with replacement LGPL libraries

The public source tree is the relinkable application material. It contains the
C++ SDK and CLI sources, axkdeck consumes the same tree from its public source
checkout, and `external/vcpkg` pins the dependency recipes. No private source is
required.

1. Check out the axklib release recursively at the revision named by the release
   manifest. For axkdeck, also check out its named public revision.
2. Verify the libsndfile and libsoxr source archives against
   `lgpl-sources.json`.
3. Replace either source archive or overlay its vcpkg port with a compatible
   modified version. Keep the same public library interface.
4. Bootstrap `external/vcpkg` and configure the desired project with the
   release triplet and `external/vcpkg/scripts/buildsystems/vcpkg.cmake`.
5. Build `axklib_shared`, `axklib_cli`, or axkdeck. The final link is performed
   from public source and the replacement static libraries.
6. Run the native tests and package runtime scan before using the rebuilt
   artifact.

For a diagnostic relink, append a harmless unique build marker to the modified
library and verify that marker in the final binary with the platform string or
symbol inspection tool. The replacement must remain API-compatible; axklib does
not bypass the library interface or prohibit modified implementations.
