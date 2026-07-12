# FAT12, ISO9660, and standalone objects

The native library exposes read-only container variants through
`axk::MediaContainer`. `axk::open_media()` detects Yamaha SFS images, FAT12
floppies, ISO9660 CD-ROM images, and standalone `FSFSDEV3SPLX` object files.
The individual `axk::FatImage`, `axk::IsoImage`, and
`axk::StandaloneObject` types are available when an application already knows
the container kind.

## Safety boundary

The FAT reader accepts FAT12 only. It checks the BPB geometry, duplicated FATs,
cluster bounds, chain termination, loops, bad and reserved cluster markers,
cross-linked files, root and subdirectory records, duplicate names, and declared
file sizes. FAT16 and FAT32 media are rejected explicitly.

The ISO reader accepts the primary ISO9660 directory form used by Yamaha media.
It checks both-endian descriptor fields, logical block geometry, directory
record boundaries, extents, cycles, duplicate names, and path components.
Multi-extent files, Joliet, Rock Ridge, and filesystem writing are outside this
API.

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
