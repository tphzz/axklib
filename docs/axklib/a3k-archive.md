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

The envelope read contract admits version 1 archives with the bounded header,
index, and payload geometry below. This authorizes the reader only; no writer
profile is defined.

The embedded `FSFSDEV3SPLX` object headers and supported current object payloads
use the normal shared sampler-object contract. Their type and embedded object
name are authoritative. The archive index path is redundant placement metadata
and cannot override an embedded object identity.

## Container Layout

All envelope integers are little-endian. Embedded Yamaha object headers retain
their documented object-specific byte order.

| Offset | Size | Reader contract |
| --- | ---: | --- |
| `0x000` | 4 | Version. The admitted profile requires `1`. |
| `0x004` | 4 | Absolute byte offset of the terminal index. |
| `0x008` | 4 | Number of 271-byte index records, including the banner record. |
| `0x00c` | 10 | Required fixed ASCII archive signature. |
| `0x016` | 48 | Reserved bytes; the reader assigns no semantics. |
| `0x046` | 16 | ASCII marker `XXXXXXXXXXXXXXXX`. |
| `0x056` | 1,024 | Reserved bytes; the reader assigns no semantics. |
| `0x456` | variable | Banner and object payload area. |
| index offset | `count * 271` | Terminal index, which must end exactly at EOF. |

Archive size and every offset are bounded to the 32-bit profile. The index
count must be between 1 and 1,024.

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

An index entry may contain `\` instead of a complete path. Axklib reports an
`a3k_index_path_incomplete` validation issue and uses the banner's
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

Valid current Sequences may carry a nonzero unused next-tick value in their
terminal end-of-track block. Axklib accepts those timelines as ordinary current
`SEQU` nodes. If a safely bounded Sequence payload cannot be decoded, package
export preserves it as an opaque node and reports
`SEQUENCE_PAYLOAD_PRESERVED_OPAQUE`. Import then requires an explicit preserve
or skip decision. Preservation retains the event bytes but does not claim MIDI
conversion, editing, or sampler playability. An opaque Sequence does not block
the remaining Programs, Sample Banks, Samples, or Wave Data in the package.

## Rejection And Limits

Opening fails when the version, magic, fixed marker, exact terminal-index
geometry, record markers, record tails, banner entry, payload extents, or
embedded Yamaha object prefix are invalid. Malformed or overlapping extents are
rejected before object decoding. Invalid redundant path text is retained as a
validation issue when the payload itself remains safely addressable.

These checks establish a bounded reader contract. They do not establish a
writer profile. New archive emission requires a separately specified and
round-trip-validated writer profile before it can become a supported operation.
