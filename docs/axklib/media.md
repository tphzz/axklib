# Supported Media Profiles

The native library exposes opened container variants through
`axk::MediaContainer`. `axk::open_media()` detects Yamaha SFS images, FAT12
floppies, ISO9660 CD-ROM images, and standalone `FSFSDEV3SPLX` object files.
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
Multi-extent files are rejected. Joliet names, Rock Ridge system-use extensions,
alternate descriptor trees, and in-place filesystem mutation are not
interpreted. A hybrid image can still open through a valid primary ISO9660 tree,
but names or metadata supplied only by those extensions are outside the API
contract. `axklib create iso` separately creates the deterministic one-group,
one-volume profile documented in [CD-ROM Images](cdrom.md).

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
files remain opaque, as do type-specific fields in `SEQU` and `PRF3`. Transfer
mode copies only recognized Yamaha object payloads; it does not silently claim
support for opaque support-file formats.

## Yamaha object layer

All object payloads use the same current-object decoders as SFS images. The
normalized object catalog can therefore be passed to the normal relationship
graph service.

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
