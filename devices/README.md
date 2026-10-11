# Native Devices

Optional USB block-device access for axklib and axkdeck. The C++23 `rawdisk`
backend is shared with the manual `rawtool`; there is no second desktop I/O
implementation. The installed C++17 interface is `<axklib/sdk/devices.hpp>`.

## Build

`AXK_BUILD_DEVICES` defaults to `OFF`; the native release presets enable it.
Windows builds also produce `axklib_helper.exe`, packaged beside the desktop
server. The helper is selected by an explicit path, not the working directory.

Linux device builds require `pkg-config` and libdbus-1 development headers
(`libdbus-1-dev` on Debian/Ubuntu). Builds with `AXK_BUILD_DEVICES=OFF` do not
require them.

For a standalone backend build from this repository's root:

```bash
cmake -S devices -B build/tmp/devices -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DRAWDISK_WERROR=ON
cmake --build build/tmp/devices
ctest --test-dir build/tmp/devices --output-on-failure
```

On Windows, `devices/build.ps1` builds with the native SDK and `/WX`, runs
file-only tests and prints the executable paths. The complete `devices`
directory may also be copied and built standalone.

## Ownership And Writes

The desktop **Open** dialog separates image browsing, individual USB devices
(including exposed V6 LUNs), SCSI2SDv6 card configuration, and SCSI2SD XML.
Device enumeration happens only after choosing Device/SCSI2SDv6 or validating XML.
Every device and configured target selection is explicit.
Refresh retains each tab's selection only while the same device connection
and geometry remain available; it never selects a replacement automatically.

Selecting a carrier in SCSI2SDv6 automatically reads only its final two 512-byte
sectors, read-only. Native admission requires `BCFG`, at least one enabled target,
recognized types, unique enabled IDs, supported sector packing, and disjoint
ranges wholly before those configuration sectors. Missing or invalid layouts
are rejected, without inferred defaults, truncation or searching other sectors.
The lower-level lossless codec and rawtool discovery remain available separately.
Target details include configured identity, exact byte bounds (end exclusive),
and logical sector size. Configuration bytes are guarded through Open and
write authorization; changing selections or cancelling releases the preview.
The V6 block layout follows the
[upstream header](https://github.com/rabbitholecomputing/SCSI2SD-util-V6/blob/f4b6de761e0a00c2fb64ce944200f5a5a1a6c949/SCSI2SD-util/SCSI2SD-util/Modules/scsi2sd.h).

Permission failures on Linux request access through UDisks2 (2.7.3 or newer)
and polkit. A running system D-Bus and desktop polkit authentication agent are
required. macOS uses Apple's `authopen`; Windows uses the packaged UAC helper.
Authorization waits are bounded to 120 seconds on Linux and macOS. The app is
not elevated and never changes device permissions or automatically unmounts
Linux filesystems. `ElevationOptions::allow = false` disables these fallbacks.

`axk::inspect_scsi2sd_xml` and the `devices.xml.inspect` API validate XML without
enumerating or opening devices. Both V5 `BoardConfig` and V6 `S2S_BoardCfg`
exports are accepted, with bounded 256 KiB input and nesting, strict enabled
geometry, and unique enabled SCSI IDs. DTDs and external entities are rejected.
XML is revalidated on connection; its dialect does not prove the connected board
version or authorize writes. The bundled private Expat parser requires no
additional download when building this directory standalone.

The independently implemented XML interpretation follows the utility exporters:
[V5 ConfigUtil](https://github.com/rabbitholecomputing/SCSI2SD/blob/7ae322c12de9e67a298ab635f16f72b595d6c456/software/scsi2sd-util/ConfigUtil.cc)
and [V6 ConfigUtil](https://github.com/rabbitholecomputing/SCSI2SD-util-V6/blob/f4b6de761e0a00c2fb64ce944200f5a5a1a6c949/SCSI2SD-util/SCSI2SD-util/Modules/ConfigUtil.m).

- Listing and connecting are explicit. Startup reads recovery metadata only;
  it never opens or recovers a physical device automatically.
- A carrier lease covers sibling USB LUNs and SCSI2SD slices. An owned target
  starts read-only. Write authorization requires exclusive-use acknowledgement
  and fresh identity, geometry, layout and metadata checks.
- The sampler and every other SCSI host must be disconnected from the carrier.
  An OS claim cannot prevent access through another physical host.
- Logical targets are limited to 9,000,000,000 bytes. Larger carrier cards may
  be used through a verified SCSI2SD layout. CD-ROM targets stay read-only.
- Initial desktop editing covers A-series SFS objects and volumes. Partition
  changes, formatting, restoration and repair are not exposed for devices.
- Parameter edits remain drafts until the normal Save action. Save, Import,
  Delete and Rename each use one native transaction; no whole-device copy or
  whole-device hash is required.

Writes are gated to Linux x64 and Windows x64. Other SDK targets remain
read-only pending manual qualification. SDK compilation alone does not qualify
physical writes. macOS does not yet have the required Disk Arbitration write
claim and remains read-only in the integrated interface.

## Interrupted Writes

The host stores a durable sparse journal of original and replacement sectors
before the first write, plus bounded metadata guards. Successful writes are
flushed, read back, validated and durably marked committed. Restoration failures
retain the journal and invalidate authorization. Desktop shutdown waits for
mutation/restoration rather than force-killing the server.

Retain application state and the journal after an uncertain result. Use
**Device operation status**, never repeat the original action blindly. Status
checking can fence an unstarted request without accessing a device. Recovery
requires explicit device selection and acknowledgement; it refuses changed
identity, layout, guards or sector bytes other than the journal's original or
replacement bytes. It never force-recovers or automatically resumes a session.
Reopen and authorize again after recovery. Access mode changes also require
ejecting and reopening; there is no standalone authorization action.

Serial numbers are not card identities. Connection identity and content guards
reduce stale-selection risk but cannot distinguish a byte-identical cloned
card. Journals cannot guarantee recovery after power loss or failing hardware.
State is bounded to 4096 operation records and metadata guards to 65,536
physical sectors; exhausted limits refuse writes before publication.

Unsaved desktop parameter drafts are retained in local application storage.
Restoration is explicit and requires the same object and base payload. Program
assignment-membership changes remain available as JSON for manual review, not
automatic replay. If local storage fails or fills up, new drafts remain in
memory for JSON export; export them before closing axkdeck. Never clear
application state while recovery is unresolved.

## Qualification

Automated tests use files and injected transports only. All physical writes
remain manually executed, including reversible empty-sector write tests.
The six-target workflow must validate new revisions with
real Windows/macOS SDKs and warnings as errors before release qualification.
