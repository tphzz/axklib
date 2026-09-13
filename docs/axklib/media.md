# Supported Media Profiles

axklib opens Yamaha SFS images, FAT12 floppies, standard FAT16 volumes and
primary-MBR FAT16 disks, EX5 disks, ISO9660 CD-ROMs, A3K volume archives,
standalone Yamaha objects and explicit object directories. The installed SDK
entry point is documented in [C++ API](cpp-api.md).

These readers implement specific supported Yamaha media profiles.
They are not general-purpose FAT or ISO libraries. An image
outside that compatibility scope may happen to use the accepted structures,
but that does not make arbitrary media a supported input contract.

## FAT12 profile

The A-series floppy profile accepts FAT12 only. It checks the BPB geometry, duplicated FATs,
cluster bounds, chain termination, loops, bad and reserved cluster markers,
cross-linked files, root and subdirectory records, duplicate names, and declared
file sizes. Directory entries use their DOS 8.3 identity; long-filename entries
are ignored. FAT32, exFAT, filesystem repair, and in-place filesystem
mutation are unsupported. `axklib create floppy` separately creates the narrow
fixed-geometry profile documented in [FAT12 Floppy Images](floppy.md).

## EX5 disk profile

The separate [EX5 Disk Images](ex5.md) profile provides directory and raw-file access, with guarded
filesystem edits on writable, structurally admitted images. It uses EX5-specific recognition and size
fields, not generic FAT16 detection. Files remain opaque and are not projected
into the A-series sampler-object catalog.

## Standard FAT16 profile

Standard FAT16 is admitted as a plain volume starting at byte zero,
or inside primary MBR partitions of type `0x04`, `0x06` or
`0x0e`. MBR regions must be nonempty, disjoint and wholly inside the image.
Each volume is read through its declared partition boundary; boot geometry
cannot borrow bytes from a neighboring partition. Extended partitions, GPT,
FAT32 and exFAT are not supported. Mixed unsupported MBR partition types are
rejected rather than silently omitted.
EX5-formatted MO media uses the distinct [EX5 removable profile](ex5.md#removable-media-profile).

Standard BPB total-sector fields and FAT16 end markers `0xfff8..0xffff`
apply here. This policy is deliberately separate from the EX5 hard-disk
profile. Both FAT copies must agree, and reachable allocation chains must be
bounded, acyclic and non-overlapping. Directory and file entries retain their
raw FAT attributes, including read-only, hidden, system and archive flags.
Names use the DOS 8.3 identity; long-name rows are not interpreted.

The Files view lists primary partitions independently. Files remain opaque;
this profile does not decode device-specific sound or sequence payloads.

### Filesystem Editing

SFS, standard FAT16 and EX5 roots can expose raw file editing when their source
is writable and allocation metadata is safe. See
[Raw Filesystem Operations](write.md#raw-filesystem-operations) for names,
conflicts, permissions and transaction guarantees. Read capability alone never
implies writable device-specific objects.

## ISO9660 profile

The ISO reader accepts the primary ISO9660 directory form used by Yamaha media.
It checks both-endian descriptor fields, logical block geometry, directory
record boundaries, extents, cycles, duplicate names, and path components.
Directory parsing reads one sector at a time and enforces limits of 16 MiB per
directory, 64 MiB of aggregate directory data, 16,384 directories, 100,000
records, 64 path components, and 64 MiB of aggregate path metadata.
Multi-extent files are rejected. Joliet names, Rock Ridge system-use extensions,
alternate descriptor trees, and in-place filesystem mutation are not
interpreted. A hybrid image can still open through a valid primary ISO9660 tree,
but names or metadata supplied only by those extensions are outside the API
contract. `axklib create iso` separately creates a deterministic one-group,
one-volume image. Partition conversion can place several source volumes in one
generated group; both profiles are documented in [CD-ROM Images](cdrom.md).

## AXK object directory profile

An `AXK_OBJECT_DIRECTORY` is either one flat host directory whose regular files
contain `FSFSDEV3SPLX` Yamaha objects, or a bounded parent containing one level
of such leaf directories. Object recognition, decoding, catalog construction,
relationship resolution, preview, audition, and package export use the same
object layer as image-backed media. Unrecognized regular support files are
ignored.

The session presents the admitted objects as one synthetic `Object directory`
volume. That scope can be exported as a `.axkvol` package, but its name and
partition index are navigation metadata; they do not recover an original
floppy volume label or partition layout.

The parent form supports Yamaha multi-floppy object sets. A split `SMPL` file
declares its complete logical Wave Data byte count, its local segment size, and
its segment offset. Axklib groups matching headers and assembles only complete,
contiguous, byte-identical segment sets. A flat leaf opens without inspecting
its siblings and remains readable for inventory and diagnostics when incomplete.
Preview, audition, or complete package export then reports that companion disks
are required. Applications can explicitly attach selected disk folders, or
explicitly request a bounded immediate-sibling search, to the existing session.
Only exact continuation segments with a normalized Yamaha header identity, even
when Yamaha changes the host filename between disks, and Wave Data objects whose
embedded names exactly satisfy active unresolved Sample member lanes, are
admitted. Unrelated sibling objects remain outside the session. This attachment
is session state and does not combine or rewrite the source directories.

The profile is intentionally read-only and bounded to 224 entries per leaf,
1,024 total entries, 16 MiB of aggregate file data, and one nested directory
level. Links, deeper nesting, case-insensitive duplicate paths, unsafe names,
and directories without a recognized object are rejected. The directory does
not recover FAT allocation, DOS directory order, deleted entries, volume labels,
or any other missing container metadata. Higher collection directories remain
navigation scopes rather than media sessions.

## A3K archive profile

An A3K `.a3k` file is a read-only PC volume archive, not an SFS image.
It contains a fixed `archive signature` header, uncompressed complete Yamaha sampler
objects, and a terminal path index. Axklib exposes the admitted objects as one
synthetic partition and one volume. Embedded Yamaha object type and name fields
are authoritative; redundant index paths remain placement metadata and
diagnostics.

Inventory loads only object prefixes and metadata needed by the catalog. Wave
Data payloads remain lazy until preview, audition, audio/SFZ export, or package
export needs them. A whole archive can be exported directly as one `.axkvol`,
but archive creation, repacking, alteration, repair, package import, and media
conversion are unsupported. See [A3K Volume Archives](a3k-archive.md) for
the bounded byte contract and support status.

## Format Documentation Map

The public format pages divide the byte contracts by layer:

| Layer or file class | Exact public contract |
| --- | --- |
| FAT12 boot sector, FAT entries, directory entries, DOS 8.3 names, and generated root filenames | [FAT12 Floppy Images](floppy.md) |
| EX5 disk descriptor, FAT16 geometry, directories and raw file reads | [EX5 Disk Images](ex5.md) |
| ISO descriptors, both path tables, directory records, raw folder names, `0000` catalogs, group-label files, and generated `Fnnn` names | [CD-ROM Images](cdrom.md) |
| A3K header, payload area, terminal index, and one-volume projection | [A3K Volume Archives](a3k-archive.md) |
| Complete `FSFSDEV3SPLX<type>` files and decoded `SMPL`, `SBNK`, `SBAC`, and `PROG` fields | [Sampler Data Structures](sampler-data.md) |
| Fresh floppy, fresh ISO, and floppy-object-to-ISO manifests | [Writer And Alteration](write.md) |
| Sampler-facing labels, duplicate disambiguation, and export filenames | [Name, Path, And Export Mapping](names-and-paths.md) |

This documentation is exact about structures that axklib reads or writes. A
file being visible to the container reader does not imply that its inner format
is decoded. The 257-record `YAMAHA.SYM` disk/file/category catalog is decoded
and synthesized; other model-specific floppy system files remain opaque, as do
PRF3 layouts other than the documented [System Files](system-files.md). The admitted current
`SEQU` timeline is documented in
[Sequence Data And MIDI Conversion](sequences.md). Transfer mode copies only
recognized Yamaha object payloads; it does not silently claim support for
opaque support-file formats.

## Yamaha object layer

A-series object payloads use the same current-object decoders as SFS images. The
normalized object catalog can therefore be passed to the normal relationship
graph service.

The installed `axk::image::open()` SDK facade uses the same media dispatcher.
SDK inventory, validation, preview, PCM, and export operations therefore accept
SFS, FAT12, ISO9660, A3K archives, standalone Yamaha objects, and AXK
object directories through one session API.

## CD menu labels

`MediaObject::group_label` and `MediaObject::volume_label` retain a value,
status, and basis:

- `confirmed` identifies a decoded Yamaha CD menu label.
- `navigation_aid` identifies a content-derived fallback chosen from the first
  suitable Program or bank/sample object.
- `raw_identifier` identifies an ISO directory name such as `F001`.

Content-derived fallbacks are display and export navigation aids only. They are
not promoted to sampler metadata. Export path mapping sanitizes path
components and adds raw volume identifiers when displayed labels collide.

## Further Reading

Use [C++ API](cpp-api.md) for installed interfaces and
[Typical Usage](typical-usage.md) for SDK and CLI examples.
