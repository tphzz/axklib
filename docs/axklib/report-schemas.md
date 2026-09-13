# Report Schemas

Commands that emit CSV or JSON write a schema index beside their reports. Each
schema has a major and minor version. Consumers must reject an unsupported major
version and may ignore unknown fields added in a compatible minor version.

Object rows include container kind, scope, object key, sampler object type and
name, payload location, decoded kind, and diagnostic fields. Relationship rows
include source, target or candidates, relationship type, quality, basis,
assignment state, and sampler-facing location. Validation rows include stable
issue code, severity, message, sampler path, and technical object key.

SFS allocation summaries report the first and second bitmap
offsets and used-cluster counts separately, whether the complete copies match,
their validity flags, selected active copy, mismatch byte and cluster counts, reconstructed usage, invalid or
extent-total-mismatched records, and cross-linked clusters. Allocation mismatch
rows identify which stored copy and comparison direction produced each
inclusive cluster range; multiple-owner rows identify both claim kinds and SFS
record IDs when present. Consumers must not collapse the two stored copies into
one comparison bitmap. Free-space and allocation-map accounting use the active
valid copy, without hiding disagreement with the other copy.

Exact exports also write a selection graph that links Wave Data files, rendered
stereo files, Samples, Sample Banks, Programs, parameters, and any
unresolved decisions.

## Object Placement And Relationships

Objects retain their physical source placement independently of sampler-facing
names. SFS placement includes partition, record ID and extent location; FAT
placement includes the short filename, directory entry and cluster chain; ISO
placement includes the raw path and extent; A3K placement includes terminal-index
path and payload offset. These identities are not interchangeable with display
names. See [Names, Paths, And Exports](names-and-paths.md).

ISO-specific metadata keeps both raw and displayed identities:

| Field | Meaning |
| --- | --- |
| `iso_raw_group`, `iso_raw_volume` | Raw ISO directory components. |
| `iso_group_label`, `iso_volume_label` | Sampler-facing labels when available. |
| `iso_extent_sector` | File extent's starting logical sector. |
| `iso_data_offset` | Absolute payload byte offset. |
| `iso_file_size` | Logical size from the ISO directory record. |
| `iso_recovery_quality` | Object-row loader classification. |

Raw placement remains available when a label is missing or a sampler-object
interpretation is unavailable. Display fallbacks do not replace physical identity.

The relationship schema uses these type names:

| Relationship | Meaning |
| --- | --- |
| `PROG_ASSIGNMENT_TO_SBAC` | Program assignment to a Sample Bank. |
| `PROG_ASSIGNMENT_TO_SBNK` | Program assignment directly to a Sample. |
| `SBAC_SLOT_TO_SBNK` | Sample Bank member row. |
| `SBNK_LEFT_MEMBER_TO_SMPL` | Sample left-member Wave Data link. |
| `SBNK_RIGHT_MEMBER_TO_SMPL` | Sample right-member Wave Data link. |

Target matching and stored-row state are separate properties. A stored Program
row can remain in the raw payload without a resolved dependency edge.
`diagnostic_category` distinguishes rows such as `program-link-bitmap`,
`sbnk-member-cache`, and `stored-assignment-missing-target`. CSV/JSON reports
retain diagnostic rows and raw selectors even when normal `info` output omits
them from the visible child list.

Package dependency closure includes effective `Known` named Sample and Sample
Bank targets regardless of Output 2. An exact, unique same-volume target remains
eligible when other volumes contain the same type and name. Ambiguous,
cross-volume-only and missing matches remain diagnostics, not inferred package
dependencies. The quality label describes the report's resolution result, not
a field stored on disk.

## Physical And Rendered Audio

Exact exports keep `_samples/physical/*.wav` (mono SMPL Wave Data) distinct from
`_samples/rendered/*.wav` (compatible linked stereo members). The optional
selection graph connects objects, relationships and WAV references and retains
unresolved decisions. See [Names, Paths, And Exports](names-and-paths.md) for
path construction.
