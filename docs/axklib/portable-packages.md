# Portable Object Packages

The axklib portable-package format moves complete Yamaha A-series object graphs
between supported SFS/HDS, Yamaha FAT12 floppy, and Yamaha ISO9660 media. A
package contains original Yamaha object payloads and a source-neutral graph. It
is not a disk image, a WAV collection, or a general backup format.

All package files use the same version 1 deterministic ZIP container and
manifest schema. The filename extension communicates the selected root to a
person; it does not select a parser or override the manifest.

## Package Kinds And Extensions

| Selected roots | Manifest `package_kind` | Extension | Meaning |
| --- | --- | --- | --- |
| One or more complete volumes | `volume` | `.axkvol` | Every admitted object placed in each volume and its complete known closure |
| One or more Programs | `program` | `.axkprg` | The Programs, assigned Sample Banks or Samples, and required Wave Data |
| One or more Sample Banks | `sbac` | `.axksbac` | Sampler-visible `B <name>` Sample Banks, their Samples, and required Wave Data |
| One or more Samples | `sbnk` | `.axksbnk` | Samples (`SBNK`) and their Wave Data dependencies |
| One or more Wave Data objects | `smpl` | `.axksmpl` | Wave Data (`SMPL`) storage objects |
| One or more Sequences | `sequence` | `.axkseq` | Byte-preserved Sequence (`SEQU`) objects with no external dependency edges |
| Different root kinds | `bundle` | `.axkpkg` | A mixed-type collection of explicitly selected roots |

Dependencies never change the extension. A Program containing SBAC, SBNK, and
SMPL nodes remains `.axkprg`, as do two selected Programs. A Program and a
Sample selected together form `.axkpkg`. Version 1 currently admits
current-format `PROG`, `SBAC`, `SBNK`, `SMPL`, and `SEQU` objects. `PRF3`,
unknown types, and non-current object profiles are rejected because their
portable dependency and relocation contracts are not admitted. A Sequence has
no admitted external dependency, so `.axkseq` contains its raw object payload
without relationship or relocation entries. Complete `.axkvol` export may
include Sequences. Optional Sequence rename changes only its current object-name
field and preserves the internal track/lane label.

Writers append the derived extension to a suffix-free output stem. A different
recognized package extension or an unrelated extension is rejected. Readers
derive the kind from `manifest.json`; renaming a valid package produces the
nonfatal `PACKAGE_EXTENSION_MISMATCH` issue without changing its kind or ID.

## CLI Workflow

Export one Program from an SFS image:

```bash
axklib package export source.hds \
  --partition 0 --group "Partition 1" --volume "Stage" \
  --root program=001 -o "Stage Piano" --format json
```

The suffix-free output becomes `Stage Piano.axkprg`. Root grammar is:

```text
volume
program=NAME
sample-bank=NAME
sample=NAME
wave-data=NAME
sequence=NAME
```

The raw object selectors `sbac`, `sbnk`, and `smpl` remain supported. `prog` is
an alias for `program`. `sample-bank` always selects an `SBAC` Sample Bank; use
`sample` or `sbnk` for an `SBNK` Sample, and `sequence` for a `SEQU` object.
Obsolete pre-release selector names are rejected. `--partition`, `--group`, and
`--volume` constrain all roots in one export command. Repeat `--root` to create
a multi-root package. Homogeneous roots keep their typed extension; mixed root
kinds use `.axkpkg`.
A selector must resolve exactly once and every required active relationship
must be known and unambiguous; otherwise no archive is published. Ambiguous
inactive Program diagnostic rows are not package content.

For an ISO Sample Bank, duplicate Sample names in other raw volumes do not make
the package ambiguous when the selected bank and exactly one matching Sample
are exact ISO9660 directory entries in the same raw volume. The closure remains
volume-local. Recovered placement, no local match, or more than one matching
Sample in the selected raw volume blocks export; package construction never
widens the closure gate to accept every `Likely` relationship.

Inspection validates the archive profile, canonical manifest, graph, declared
entry sizes, and package ID while reading only bounded metadata and the
manifest:

```bash
axklib package inspect "Stage Piano.axkprg" --format json
```

Its JSON reports `"payloads_verified":false`. Full verification additionally
reads and hashes every payload, decodes every object, recomputes normalized and
waveform identities, and validates closure and relocation descriptors:

```bash
axklib package verify "Stage Piano.axkprg" --format json
```

Successful full verification reports `"payloads_verified":true`. Import always
performs full verification even when the caller previously inspected a file.
Inspection reports the exact uncompressed object total as
`total_payload_bytes` in CLI JSON and `totalPayloadBytes` through the server
contract. This is package content size, not the amount a target image will
allocate.

Plan before applying:

```bash
axklib package plan-import target.hds "Stage Piano.axkprg" \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported"}' \
  --format json
```

Each plan allocation reports `additional_allocated_bytes` in CLI JSON and
`additionalAllocatedBytes` through the server contract. That value includes
payload rounding, continuation metadata, category-directory growth, and other
target-format infrastructure. It is the authoritative space estimate shown by
axkdeck. Category directories grow during a valid SFS import; physical cluster
or object-ID exhaustion remains a blocking conflict.

Plans also report `program_assignment_adjustments` in CLI JSON and
`programAssignmentAdjustments` through the server contract. These are
nonblocking, row-specific decisions for unresolved Program assignments that
would otherwise acquire an exact type-and-name target in the destination. Each
entry identifies the imported or existing Program, assignment ordinal, target
type and name, destination scope, reason, and `clear-assignment` disposition.
Axkdeck shows these decisions before enabling import.

Apply the exact plan through the same request:

```bash
axklib package import target.hds "Stage Piano.axkprg" \
  --destination '{"package":0,"root":0,"partition":0,"volume":"Imported"}' \
  -o imported.hds --format json
```

The CLI import shown above never edits the source image. It builds and validates
a complete temporary image, then publishes atomically. Existing output paths
are refused unless `--overwrite` is explicit. A plan with conflicts is reported
with exit code 3 and is not applied.

An authenticated writable SFS image session has a separate in-place workflow
for axkdeck. `images.package_import.plan` fully verifies one package and returns
an owner-bound, expiring plan tied to the current image revision and retained
image identity. `images.package_import` applies that exact plan through the
alteration journal, validates the resulting image, and refreshes the retained
session. `images.package_import.release` explicitly discards an unused plan.
This session workflow preserves crash recovery and atomic rollback guarantees;
it is not the CLI's separate-output publication model.

`images.package_export` exports one to 1,024 exact roots from any readable
image session. A root is either a selected volume or an opaque session object
ID with kind `PROGRAM`, `SBAC`, `SBNK`, or `SMPL`. Homogeneous roots use their
specific package extension regardless of count; mixed root kinds use `.axkpkg`.
Shared dependencies are included once. A workspace destination publishes
through the sandbox. A
download destination creates a short-lived, owner-scoped retained file for a
native client to stream and explicitly delete. The package contents and
identity are identical in both cases.

### Destination Objects

Every package root needs exactly one `--destination` JSON object. `package` and
`root` are zero-based indexes in command-line package order and manifest root
order.

| Target | Required destination fields |
| --- | --- |
| Existing SFS volume | `package`, `root`, `partition`, `volume` |
| New SFS volume | Same fields plus `"create":true` |
| Yamaha FAT12 root | `package`, `root`, `partition:0`, `volume:"FAT root"` |
| Existing ISO raw volume | `package`, `root`, `partition:0`, `group`, `volume`, `raw_group`, `raw_volume` |
| New ISO raw volume | Same labels and raw group plus `"create":true`; omit `raw_volume` or name the next contiguous `Fnnn` value |

SFS destinations do not accept `group`, `raw_group`, or `raw_volume`. FAT12 has
one namespace and accepts no group or creation fields. ISO labels must match
the existing Yamaha catalogs. A new ISO volume must be the next contiguous
raw volume under its raw group.

An ISO manifest may create an object-empty `F001` volume for use as a package
import staging target. Populate that image before distribution; an empty ISO is
not a hardware-promoted standalone writer profile.

All destinations in one import request are resolved against the input image.
One root can create and populate a new SFS volume, but another root in the same
request cannot refer to that not-yet-published volume. Import a multi-root
bundle into the new volume, or publish the volume first and add another package
in a second atomic import.

Example FAT destination:

```json
{"package":0,"root":0,"partition":0,"volume":"FAT root"}
```

Example existing ISO destination:

```json
{"package":0,"root":0,"partition":0,"group":"KEYS","volume":"PIANO","raw_group":"46DEF120","raw_volume":"F001"}
```

### Explicit Renames

Same-name, different-identity objects conflict. Resolve an intended collision
with a UTF-8 JSON rename-map file:

```json
[
  {"package": 1, "node_id": "n-<lowercase-sha256>", "name": "Piano Copy"}
]
```

Pass it with `--rename-map renames.json`. Each entry names exactly one package
node. Destination names contain 1 to 16 ASCII bytes. Renaming one node also
relocates every admitted raw name or link field bound to graph edges; it does
not authorize edits to unknown bytes.

## Target Reuse Policies

Reuse requires the same object name and normalized identity. PCM or audio hash
equality alone is diagnostic and never authorizes reuse.

| Target | SMPL reuse scope | Behavior outside that scope |
| --- | --- | --- |
| SFS/HDS | Destination volume | Another volume or partition receives another SMPL |
| Yamaha FAT12 | Whole FAT root | There is no second volume namespace |
| Yamaha ISO9660 | One `<raw_group>/<raw_volume>` folder | Another `Fnnn` folder receives another SMPL |

Non-SMPL objects are destination-owned and are not shared across SFS volumes.
SFS partition-wide sharing is unavailable in v1. The import policy is fixed to
volume-local reuse, and SFS objects are never shared across partitions.

Allocation output uses the target's native units. SFS and FAT plans report
clusters. ISO plans report 2048-byte payload sectors and the exact projected
final image sector and byte counts. Reimporting the same package into the same
scope produces reuse actions and zero additional payload allocation.

## Conflict Codes

Planning is read-only and returns every independently detectable conflict.
Conflicts block the complete plan.

| Codes | Meaning and resolution |
| --- | --- |
| `DESTINATION_ROOT_INVALID`, `DESTINATION_ROOT_MISSING`, `DESTINATION_ROOT_DUPLICATE` | Correct the package/root indexes and supply exactly one mapping per root. |
| `PACKAGE_RENAME_INVALID`, `RENAME_NODE_INVALID`, `RENAME_NODE_DUPLICATE`, `RENAME_NAME_INVALID` | Correct the rename-map node, uniqueness, or 1-to-16-byte ASCII destination name. |
| `SFS_DESTINATION_INVALID`, `SFS_DESTINATION_PARTITION_MISSING`, `SFS_DESTINATION_MISSING`, `SFS_DESTINATION_ALREADY_EXISTS`, `SFS_DESTINATION_POLICY_CONFLICT` | Correct the SFS partition, volume, and `create` policy. |
| `SFS_OBJECT_NAME_INVALID`, `SFS_PROGRAM_SLOT_INVALID`, `SFS_TARGET_NAME_AMBIGUOUS`, `SFS_NAME_CONFLICT` | Correct names, disambiguate target content, or provide an explicit rename. |
| `SFS_OBJECT_ID_EXHAUSTED`, `SFS_CLUSTER_EXHAUSTED`, `SFS_ROOT_DIRECTORY_MISSING`, `SFS_ROOT_DIRECTORY_CAPACITY_EXHAUSTED`, `SFS_CATEGORY_MISSING`, `SFS_DIRECTORY_CAPACITY_EXHAUSTED`, `SFS_ALLOCATION_INVALID` | Select a target with valid SFS metadata and sufficient IDs, clusters, and directory capacity. |
| `FAT12_DESTINATION_INVALID`, `FAT12_PROFILE_UNSUPPORTED` | Use partition 0 and `FAT root` on an admitted root-only Yamaha FAT12 image. |
| `FAT12_OBJECT_NAME_INVALID`, `FAT12_PROGRAM_SLOT_INVALID`, `FAT12_TARGET_NAME_AMBIGUOUS`, `FAT12_NAME_CONFLICT` | Correct names, ambiguity, or collisions. |
| `FAT12_ROOT_ENTRY_EXHAUSTED`, `FAT12_CLUSTER_EXHAUSTED` | Select an image with sufficient FAT root entries and data clusters. |
| `ISO9660_DESTINATION_INVALID`, `ISO9660_DESTINATION_MISSING`, `ISO9660_DESTINATION_ALREADY_EXISTS`, `ISO9660_DESTINATION_LABEL_MISMATCH`, `ISO9660_DESTINATION_POLICY_CONFLICT` | Correct labels, raw identifiers, existence, and `create` policy. |
| `ISO9660_RAW_VOLUME_ALLOCATION_INVALID`, `ISO9660_GROUP_LABEL_CONFLICT`, `ISO9660_LABEL_METADATA_MISSING`, `ISO9660_PROFILE_UNSUPPORTED` | Use the documented Yamaha ISO profile and contiguous `Fnnn` allocation. |
| `ISO9660_OBJECT_NAME_INVALID`, `ISO9660_PROGRAM_SLOT_INVALID`, `ISO9660_TARGET_NAME_AMBIGUOUS`, `ISO9660_NAME_CONFLICT` | Correct object names, ambiguity, or collisions. |
| `ISO9660_DIRECTORY_CAPACITY_EXHAUSTED` | Reduce content so each narrow-profile directory remains within one sector. |
| `ISO9660_SECTOR_CAPACITY_EXHAUSTED` | Reduce image content below the ISO9660 32-bit sector-extent limit. |
| `PLANNED_CANONICAL_OBJECT_MISSING` | The request cannot form one deterministic shared incoming object; rebuild the plan from verified packages. |
| `TARGET_ADAPTER_UNSUPPORTED` | Use SFS/HDS, the admitted Yamaha FAT12 profile, or the admitted Yamaha ISO9660 profile. |

Validation issues already present in a target are also copied into the conflict
list. Fix or replace an invalid target rather than suppressing those issues.
`ISO9660_CROSS_VOLUME_DUPLICATE` is a nonfatal warning: it explains the required
copy when equal content is imported into different raw volumes.

## Normative Version 1 Container

The machine-readable manifest shape is
[`portable-package-v1.schema.json`](portable-package-v1.schema.json). The JSON
Schema describes field syntax. The semantic requirements below are also
normative and are enforced by axklib verification.

```text
manifest.json
payloads/sha256/<64-lowercase-hex>.bin
```

Archive requirements:

- ZIP32, stored method only, UTF-8 flag set, no encryption, ZIP64, comments,
  extra fields, data descriptors, platform attributes, or trailing bytes;
- `manifest.json` is the first logical entry and all entries are in bytewise
  lexical UTF-8 path order;
- fixed DOS timestamp `1980-01-01 00:00:00`;
- safe relative paths only: no empty component, `.`, `..`, leading slash,
  trailing slash, backslash, NUL, directory entry, symlink, or device;
- every non-manifest entry is declared exactly once and no undeclared entry is
  present;
- canonical compact UTF-8 JSON with lexically ordered object keys, integer
  numbers only, LF line ending, and exactly one trailing newline.

The package ID is lowercase SHA-256 of the canonical manifest after removing
only the `package_id` member. Re-encoding a conforming logical manifest and
payload set therefore preserves its package ID even if a future container
writer changes permitted physical encoding.

Default hard limits are 4096 entries, 64 MiB per entry, 512 MiB total expanded
bytes, 512 MiB archive bytes, 16 MiB central-directory bytes, and 8 MiB
`manifest.json`. Inspection applies central-directory and manifest limits before
allocation and does not read payload bodies. Full verification applies every
limit before decoding payloads.

## Graph And Identity Contract

Roots contain package-local node IDs. Every non-root node must be reachable
from a root through one of these admitted roles:

- `PROG_ASSIGNMENT_TO_SBAC`
- `PROG_ASSIGNMENT_TO_SBNK`
- `SBAC_SLOT_TO_SBNK`
- `SBNK_LEFT_MEMBER_TO_SMPL`
- `SBNK_RIGHT_MEMBER_TO_SMPL`

Object nodes and relationships are sorted by their deterministic IDs. Required
edges must have one exact target matching the name and type encoded in the raw
object. Multiple roots share a node and payload only when their complete object
identity is the same.

`payload_sha256` covers every original payload byte. `normalized_sha256` covers
the same bytes after clearing only registered destination-assigned relocation
bits. For SMPL, `semantic_sha256` covers decoded waveform/storage semantics and
exact PCM; `audio_sha256` covers logical PCM and is diagnostic. Names and all
unknown or reserved bytes remain identity-significant.

Relocation descriptors declare offset, width, expected source bytes, optional
bit mask, role, and graph edges. Verification recomputes descriptors from the
admitted object profile. Import may change only the object name and registered
relocation ranges needed for destination links, Program membership, or group
membership. Any other byte change fails before publication.

Current `SBAC_SLOT_HANDLE` and `PROG_ASSIGNMENT_HANDLE` words are source-local
state. Import writes the admitted zero representation while preserving the
bound graph edge and its ordinal. The Program rule applies only to named
kind-`0x10` direct-SBNK and kind-`0x11` SBAC assignment rows. Empty rows, other
assignment kinds, reserved bytes, and undeclared tail words remain
identity-significant.

Program closure retains Known target rows classified as active, source-load,
or visible-off. The visible-off case is required because an imported zero
destination handle re-parses in that state even though the sampler can still
load the named assignment. Known Program edges remain required dependencies.
Ambiguous visible-off diagnostics, duplicate inactive rows, unresolved targets,
and ambiguous targets do not become dependency edges.

Any package containing a Program may preserve a stored active assignment row
whose exact same-volume target is absent. The complete row remains
identity-significant, its source-local `PROG_ASSIGNMENT_HANDLE` is normalized to
zero, and it has no relationship edge. This records source state without
inventing a dependency. It does not prevent exporting one Program, a partial
Program selection, every Program in a volume, or a complete volume.

Import never promotes that edge-less row merely because an exact type-and-name
target exists or is introduced in the destination. Instead, the plan emits one
`UNRESOLVED_PROGRAM_ASSIGNMENT_COLLISION` adjustment and atomically clears the
complete 0x38-byte assignment row. This applies both to a Program arriving in
the package and to a zero-handle unresolved row in an existing destination
Program when the import introduces the matching lower-level object. The Sample
Bank, Sample, and Wave Data import remains valid and is not blocked by the
higher-level inconsistency. Other Program rows are unchanged. Apply verifies
the cleared row before publication or in-place journal commit on SFS/HDS,
FAT12, and ISO9660 targets.

Package closure does not use a trailing `*` alone as a relationship-state
signal: when an exact target including the suffix exists, that exact object is
the dependency. This rule does not define Yamaha Duplicate state. Yamaha uses
`*` when naming duplicated Samples, but the name alone is not a portable
relationship-state flag.

## SDK Surface

Include `axklib/sdk.hpp`. `portable_package::export_from` exports and atomically
publishes one package. `portable_package::open` performs bounded manifest
inspection; call `verify` before treating payloads as trusted.
`package_import_plan::create` fully verifies every package and returns owned
warnings, conflicts, actions, Program-assignment adjustments, allocation
deltas, and a stable plan ID. `apply` rejects invalid or stale plans and
publishes a separate output image. `package_import_summary::adjustment_count`,
`package_import_plan::adjustments()`, and
`package_import_result::adjustment_count` expose the same decisions without
requiring JSON.

The installed SDK uses owned C++17 values and PIMPL handles. It does not expose
CLI11, JSON, ZIP, Yamaha parser, or allocation-engine types. Package handles and
plans are move-only. Operations accept `operation_context` for cancellation and
progress, follow the SDK thread rules, and contain implementation exceptions.

## Compatibility

Schema version `1.0` is exact in the current reader. Unknown or missing fields,
unsupported versions, and changed required semantics are rejected. A future
minor-compatible reader may define additional optional behavior, but version 1
archives cannot add undeclared fields and remain valid under the current exact
schema. A future incompatible format uses a new major version.

Portable packages are source-neutral, but target imports remain limited to the
documented Yamaha SFS, FAT12, and ISO9660 profiles. The format does not claim
compatibility with arbitrary ZIP files, general FAT12 volumes, general ISO9660
images, other Yamaha generations, or third-party backup formats.
