# A3K Volume Archives

Axklib reads `.a3k` files produced for a legacy A3K PC workflow. These files use
a fixed archive signature. They are not Yamaha SFS disk images and they are not
ZIP, LZH, or ARC files. One archive contains a banner, a terminal index, and
uncompressed complete Yamaha sampler objects.

Support is intentionally read-only. Axklib can inventory, validate, audition,
preview, export audio and SFZ, convert Sequences to MIDI, and export selected
or whole-volume portable packages. It does not create, repack, alter, import
into, or repair `.a3k` archives.

## Support Status

The envelope read contract is **Strong**. All 24 observed archives use the same
header and index geometry. This status authorizes the bounded reader only; no
writer profile is defined.

The embedded `FSFSDEV3SPLX` object headers and supported current object payloads
use the normal shared sampler-object contract. Their type and embedded object
name are authoritative. The archive index path is redundant placement metadata
and cannot override an embedded object identity.

The maintained corpus contains 24 archives and 2,924 objects:

| Object | Count |
| --- | ---: |
| Program (`PROG`) | 113 |
| Sample Bank (`SBAC`) | 160 |
| Sample (`SBNK`) | 1,375 |
| Wave Data (`SMPL`) | 1,270 |
| Sequence (`SEQU`) | 6 |

## Container Layout

All envelope integers are little-endian. Embedded Yamaha object headers retain
their documented object-specific byte order.

| Offset | Size | Reader contract |
| --- | ---: | --- |
| `0x000` | 4 | Version. The admitted profile requires `1`. |
| `0x004` | 4 | Absolute byte offset of the terminal index. |
| `0x008` | 4 | Number of 271-byte index records, including the banner record. |
| `0x00c` | 10 | Required fixed ASCII archive signature. |
| `0x016` | 48 | Reserved bytes; observed corpus files use zeroes. |
| `0x046` | 16 | ASCII marker `XXXXXXXXXXXXXXXX`. |
| `0x056` | 1,024 | Reserved bytes; observed corpus files use zeroes. |
| `0x456` | variable | Banner and object payload area. |
| index offset | `count * 271` | Terminal index, which must end exactly at EOF. |

The reader does not assign semantics to the reserved header bytes and does not
use their observed zero values for writing. Archive size and every offset are
bounded to the 32-bit profile. The index count must be between 1 and 1,024.

## Index Records

Each terminal index record is 271 bytes:

| Record offset | Size | Field |
| --- | ---: | --- |
| `0x000` | 256 | NUL-terminated, zero-padded printable ASCII path. |
| `0x100` | 2 | `00 00` for the banner; `01 00` for an object. |
| `0x102` | 4 | Absolute payload offset, little-endian. |
| `0x106` | 4 | Payload byte count, little-endian. |
| `0x10a` | 5 | Required tail `01 00 00 00 00`. |

Record zero must be `/A3kFileInfo.txt`, start at `0x456`, and remain within the
64 KiB banner bound. Object entries must begin before the index, contain at
least the shared 66-byte Yamaha object header, and remain wholly before the
index. Payload ranges may not overlap.

Object paths normally have this form:

```text
Volume name \TYPE\Object name
```

Some observed archives contain `\` instead of a complete path. Axklib reports
an `a3k_index_path_incomplete` validation issue and uses the banner's
`Volume Name` value as a navigation aid. A complete path that disagrees with
the embedded type or name produces `a3k_index_identity_mismatch`; the embedded
identity wins. Conflicting volume names produce a validation issue instead of
silently splitting one archive into several volumes.

## Axklib Projection

An admitted archive is exposed as media kind `a3k-archive` and wire kind
`A3K_ARCHIVE`. Its content tree has one synthetic partition index (`0`)
and one volume. That numeric index is an API addressing convention, not
recovered SFS partition metadata.

The volume label is selected in this order:

1. One consistent complete volume name from object index paths: `confirmed`.
2. The banner `Volume Name`: `navigation_aid`.
3. The host filename stem: `navigation_aid`.

Wave Data payloads remain lazy during metadata inventory. Preview, audition,
audio export, SFZ export, and package export read exact indexed ranges only when
needed. A whole-volume package is exported through the normal **Export
package...** action and receives `.axkvol`; the partition-level batch-volume
workflow is not advertised because an archive has no multi-volume parent.

Four corpus archives contain valid current Sequences whose terminal
end-of-track block stores a nonzero unused next-tick value. Axklib accepts these
timelines and exports them as ordinary current `SEQU` nodes. The Sequence in
`nordmicrodrums#1.a3k` is structurally damaged and still fails semantic timeline
decoding. Direct byte comparison confirms a duplicated 1 KiB region at Sequence
object offset `0x9000`, followed by omitted bytes before the stored stream
resumes. The same malformed payload SHA-256 is retained through A3K projection,
target-image storage, and portable-package export; this is source corruption,
not a decoder size-policy mismatch. Volume and Sequence package export preserve
that object as an opaque node and report
`SEQUENCE_PAYLOAD_PRESERVED_OPAQUE`. Import then requires an explicit preserve
or skip decision. Preservation retains the event bytes but does not claim MIDI
conversion, editing, or sampler playability. The malformed Sequence does not
block the remaining Programs, Sample Banks, Samples, or Wave Data in the volume
from being packaged or imported.

## Rejection And Limits

Opening fails when the version, magic, fixed marker, exact terminal-index
geometry, record markers, record tails, banner entry, payload extents, or
embedded Yamaha object prefix are invalid. Malformed or overlapping extents are
rejected before object decoding. Invalid redundant path text is retained as a
validation issue when the payload itself remains safely addressable.

These checks establish a bounded reader contract. They do not establish a
writer profile. New archive emission requires a separately specified and
round-trip-validated writer profile before it can become a supported operation.
