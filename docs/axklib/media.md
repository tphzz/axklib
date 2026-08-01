# Supported Media Profiles

The native library exposes opened container variants through
`axk::MediaContainer`. `axk::open_media()` detects Yamaha SFS images, FAT12
floppies, ISO9660 CD-ROM images, standalone `FSFSDEV3SPLX` object files, and
AXK object directories.
The individual `axk::FatImage`, `axk::IsoImage`, and
`axk::StandaloneObject` types are available when an application already knows
the container kind.

These readers implement the narrow profiles needed by maintained Yamaha
A-series media. They are not general-purpose FAT or ISO libraries. An image
outside that compatibility scope may happen to use the accepted structures,
but that does not make arbitrary media a supported input contract.

## FAT12 profile

The FAT reader accepts FAT12 only. It checks the BPB geometry, duplicated FATs,
cluster bounds, chain termination, loops, bad and reserved cluster markers,
cross-linked files, root and subdirectory records, duplicate names, and declared
file sizes. Directory entries use their DOS 8.3 identity; long-filename entries
are ignored. FAT16, FAT32, exFAT, filesystem repair, and in-place filesystem
mutation are unsupported. `axklib create floppy` separately creates the narrow
fixed-geometry profile documented in [FAT12 Floppy Images](floppy.md).

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

## Format Documentation Map

The public format pages divide the byte contracts by layer:

| Layer or file class | Exact public contract |
| --- | --- |
| FAT12 boot sector, FAT entries, directory entries, DOS 8.3 names, and generated root filenames | [FAT12 Floppy Images](floppy.md) |
| ISO descriptors, both path tables, directory records, raw folder names, `0000` catalogs, group-label files, and generated `Fnnn` names | [CD-ROM Images](cdrom.md) |
| Complete `FSFSDEV3SPLX<type>` files and decoded `SMPL`, `SBNK`, `SBAC`, and `PROG` fields | [Sampler Data Structures](sampler-data.md) |
| Fresh floppy, fresh ISO, and floppy-object-to-ISO manifests | [Writer And Alteration](write.md) |
| Sampler-facing labels, duplicate disambiguation, and export filenames | [Name, Path, And Export Mapping](names-and-paths.md) |

This documentation is exact about structures that axklib reads or writes. A
file being visible to the container reader does not imply that its inner format
is decoded. Floppy support files such as `YAMAHA.SYM` and model-specific system
files remain opaque, as do type-specific fields in `PRF3`. The admitted current
`SEQU` timeline is documented in
[Sequence Data And MIDI Conversion](sequences.md). Transfer mode copies only
recognized Yamaha object payloads; it does not silently claim support for
opaque support-file formats.

## Yamaha object layer

All object payloads use the same current-object decoders as SFS images. The
normalized object catalog can therefore be passed to the normal relationship
graph service.

The installed `axk::image::open()` SDK facade uses the same media dispatcher.
SDK inventory, validation, preview, PCM, and export operations therefore accept
SFS, FAT12, ISO9660, standalone Yamaha objects, and AXK object directories
through one session API.

## CD menu labels

`MediaObject::group_label` and `MediaObject::volume_label` retain a value,
status, and basis:

- `confirmed` identifies a decoded Yamaha CD menu label.
- `navigation_aid` identifies a content-derived fallback chosen from the first
  suitable Program or bank/sample object.
- `raw_identifier` identifies an ISO directory name such as `F001`.

Content-derived fallbacks are display and export navigation aids only. They are
not promoted to sampler metadata. `structured_object_paths()` sanitizes path
components and adds raw volume identifiers when displayed labels collide.

## Example

```cpp
#include <axklib/media.hpp>

auto media = axk::open_media("library.iso");
if (!media) {
  throw std::runtime_error(axk::render_error(media.error()));
}

auto objects = media->objects();
if (!objects) {
  throw std::runtime_error(axk::render_error(objects.error()));
}

auto paths = axk::structured_object_paths(*objects);
```
