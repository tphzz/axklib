# EX5 FAT16 Disk Images

EX5 hard disks and removable media use different FAT16-based layouts. Hard
disks have an EX descriptor and a prefixed boot record; removable media has a
boot record at byte zero. Neither shares the A-series SFS allocation layout.
This page describes the containers, not EX5 sound or sequence payloads.
Software capabilities are listed in [Media Profiles](media.md).

## Removable-media profile

EX5 removable-media volumes with boot OEM `YAMAHA??` and type label `FAT16`
start at byte
zero and use ordinary BPB sector-count fields, unlike the hard-disk geometry
below. Their FAT chains retain the EX5 `0xffff`-only end marker.

The removable layout can declare 65,525 data clusters, one beyond ordinary
FAT16 classification. For example, 4,194,176 sectors with one reserved sector,
two 256-sector FATs, 512 root entries and 64 sectors per cluster produce this
boundary. The FAT still must address every declared cluster. This EX-specific
case does not turn a standard FAT16 volume into FAT32.

Directories use DOS 8.3 identities and native FAT attributes. The image filename
and editable volume label do not identify which layout is present.

### One-sector capacity mismatch

An EX removable volume can declare exactly one 512-byte sector beyond the
physical image. The bounded case described here has sector-aligned storage,
EX5 OEM/type markers, a valid FAT16 boot signature, 512-byte sectors, one
reserved sector, two matching FATs, 512 root entries and 4..64 sectors per
cluster. This shape alone cannot distinguish a formatter mismatch from a
truncated file. Other short images require their own explanation.

Declared geometry still defines chain addresses. Boot records,
both FATs, the root and all reachable directories must be completely readable;
missing directory metadata still rejects the image. File ranges that physically
exist remain readable, including logical payload ending before the missing
sector in an incomplete cluster. A file whose logical bytes cross the image end
has unavailable data and cannot be exported completely.
Missing bytes are never synthesized.

Safe allocation requires consistent metadata and complete, physically backed
clusters. This also
applies to directory growth and padded file writes. Writes must remain bounded by the physical image length. An incomplete final cluster cannot supply free space even if its FAT
entry is free. When the absent sector lies only in unused tail space, no data
cluster needs excluding.

Excluding an incomplete cluster from a host-side allocation calculation does
not change its on-disk FAT entry or repair the BPB. The sampler may still use
the declared geometry. Subsequent device writes cannot be assumed to honor
host-side access restrictions.

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

## Directory And Chain Rules

Names use the short directory identity. Long-name entries, deleted records and
unreferenced allocation do not establish additional EX5 files. Overallocated
chains can contain unused capacity; logical reads end at the directory entry's
file size. Cycles, cross-links, out-of-range successors and chains shorter than
the declared file size are inconsistencies.

Filesystem bytes and EX5 sound/sequence payloads are separate formats. The
latter's inner structures are not specified by this page.
