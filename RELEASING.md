# Manual Release Procedure

axklib releases are published deliberately from the manually dispatched
`Native CI` workflow. Pushing a tag does not start or publish a release.

1. Update the version in `CMakeLists.txt`, `library/CMakeLists.txt`,
   `apps/cli/CMakeLists.txt`, and `vcpkg.json`. Run the complete local quality
   gates and commit the result.
2. Create and push an annotated `vX.Y.Z` tag whose version exactly matches the
   project version:

   ```bash
   git tag -a vX.Y.Z -m "axklib vX.Y.Z"
   git push origin vX.Y.Z
   ```

3. Dispatch `Native CI` against that exact tag with release configuration and
   macOS notarization enabled:

   ```bash
   gh workflow run native.yml --ref vX.Y.Z \
     -f debug=false -f notarize_macos=true
   ```

4. Wait for the format gate, all native matrix jobs, and the universal macOS
   job to pass. Record the successful run ID, then download its artifacts:

   ```bash
   rm -rf build/release-assets
   gh run download RUN_ID --dir build/release-assets
   ```

   The download must contain these five artifact directories:

   - `axklib-linux-x64-release`
   - `axklib-linux-arm64-release`
   - `axklib-windows-x64-release`
   - `axklib-windows-arm64-release`
   - `axklib-macos-universal-release`

5. Verify every archive against the adjacent `SHA256SUMS` file. The release
   asset set is one `.tar.gz` for each Linux architecture, one `.zip` for each
   Windows architecture, one universal macOS `.zip`, and all five checksum
   files. Do not publish per-architecture macOS slice artifacts.
6. Create the GitHub release only after inspecting the extracted distributions
   and confirming that each contains the CLI, shared SDK, headers, CMake package
   files, licenses, and component SBOMs:

   ```bash
   gh release create vX.Y.Z build/release-assets/*/* \
     --verify-tag --generate-notes --title "axklib vX.Y.Z"
   ```

The workflow rejects a selected tag whose name does not match the CMake project
version. A failed or incomplete run is not a release candidate; fix the source,
create a new version tag, and repeat the procedure.
