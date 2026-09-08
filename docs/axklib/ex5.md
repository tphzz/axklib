# EX5 Disk Images

The EX5 disk profile supports read-only filesystem access: directory listing,
DOS 8.3 file names, allocation chains, exact file bytes and bounded range reads.
It does not decode EX5 sound or sequence payloads, generate EX5 images, repair
allocation, or modify existing images. This is not generic FAT16 support.

## Recognition

The disk descriptor at byte `0x210` contains the exact 16-byte signature
`SY1200 V0.0.0   `. The boot record is at sector 256, with 512-byte sectors.
Recognition takes precedence over residual SFS headers. A recognized EX5
descriptor with invalid geometry is rejected rather than retried as SFS.

## Geometry

All multibyte boot fields below are little-endian. Offsets are relative to
the boot record, not the beginning of the image.

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| `0x0b` | 2 | Bytes per sector; supported value 512 |
| `0x0d` | 1 | Sectors per cluster; power of two, at most 64 |
| `0x0e` | 2 | Reserved sectors; 2 |
| `0x10` | 1 | FAT copies; 2 |
| `0x11` | 2 | Root directory entry count; whole sectors of 32-byte entries |
| `0x13` | 2 | Data cluster count, **not** a sector count |
| `0x15` | 1 | Media descriptor; `0xf8` |
| `0x16` | 2 | Sectors per FAT |
| `0x18` | 2 | Sectors per track; 32 |
| `0x1a` | 2 | Heads; 8 |
| `0x20` | 4 | Sector capacity excluding the 256-sector prefix, two reserved sectors and root directory; includes both FATs |
| `0x36` | 8 | `FAT16` followed by spaces |
| `0x1fe` | 2 | Bytes `55 aa` |

The first FAT begins at sector 258. The second immediately follows it; the
root follows both FATs, and the data area follows the root. Data cluster 2 is
the first data cluster. FAT entries are 16-bit little-endian values.
Only `0xffff` terminates an EX5 chain. Values such as `0xfff8` are cluster
numbers when they lie inside the declared data area, unlike generic FAT16
end markers. The maximum admitted data cluster count is `0xfffd`, keeping
`0xffff` outside the data region. Zero and one are invalid reachable successors.

The declared data area must fit both the capacity and physical image. Both FAT
copies must agree and address every declared cluster. Generic FAT BPB precedence
between the 16-bit and 32-bit total-sector fields does not apply.
Trailing sectors outside the addressable data region are not file data.

## Native Access

`open_media()` returns `MediaKind::ex5_disk` backed by `FatImage`, whose geometry
has `FatProfile::ex5_disk`. `FatGeometry::total_sectors` is the normalized end
of the addressable filesystem, measured from image sector zero; it is not the
raw boot field at `0x20`. `boot_offset`, `fat_offset`, `root_offset`, and
`data_offset` are absolute byte offsets.

- `directories()` includes empty and nested directories, excluding dot entries.
- `files()` lists regular files with their sizes, physical directory-entry
  offsets and allocation chains.
- `read_file()`, `read_file_prefix()` and `read_file_range()` read exact file
  bytes across fragmented chains. Prefer bounded range reads for large files.
- `build_content_tree()` exposes directories and files, not synthesized
  A-series Programs, Samples or Wave Data. The sampler-object catalog is empty.

The reader rejects cyclic or cross-linked reachable chains, truncated files,
out-of-range successors, duplicate names and unsafe path components.
Names use the short directory identity; long-name entries are ignored. Deleted
entries and unreferenced allocation are not recovered. Overallocated file chains
are retained, while payload reads stop at the declared file size.

Resource limits are 16 MiB per directory, 64 MiB of aggregate directory bytes,
16,384 directories, 100,000 entries, 64 path components and 64 MiB of aggregate
path metadata. These are reader resource limits, not EX5 authoring limits.

EX5 disk access does not imply support for EX5 floppies, CD-ROM layouts,
alternate sector sizes, or the EX5 file payload formats.
