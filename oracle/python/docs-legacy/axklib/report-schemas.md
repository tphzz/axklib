# Report Schemas

axklib commands write CSV and JSON reports for inventory, object decoding,
relationships, coverage, validation, and exact export. Reports are intended to be
machine-readable: every generated schema manifest records column names, inferred
column types, quality columns, issue-code columns, object-reference columns, row
counts, and compatibility notes.

## Schema Manifests

Report schema manifests use schema version `1.0` and are written as JSON. The
manifest model is `ReportSchemaManifest`.

| Field | Meaning |
| --- | --- |
| `report_name` | Name of the report file family. |
| `schema_version` | Schema manifest version. |
| `row_count` | Number of rows used to infer the manifest. |
| `columns` | Column-level schema entries. |
| `quality_counts` | Counts of values in known quality columns. |
| `issue_code_counts` | Counts of stable diagnostic or validation codes. |
| `object_type_counts` | Counts of object type values when present. |
| `quality_columns` | Columns treated as quality labels. |
| `issue_code_columns` | Columns treated as issue-code columns. |
| `object_ref_columns` | Columns treated as object or source references. |
| `source_command` | Optional command text supplied by the caller. |
| `library_version` | Optional axklib version supplied by the caller. |
| `semantic_notes` | Report-level notes. |
| `replacement_notes` | Migration notes for replaced workflows. |

Column schema entries use `ReportColumnSchema`:

| Field | Meaning |
| --- | --- |
| `name` | Column name. |
| `type` | Inferred type: `null`, `boolean`, `integer`, `number`, `object`, `array`, `string`, or `mixed`. |
| `required` | True when every row contains the column. |
| `nullable` | True when the column can be empty or absent. |
| `semantic_notes` | Column-specific interpretation. |
| `deprecation_notes` | Optional migration note. |

## Common Column Families

Quality columns:

```text
quality
extraction_quality
match_quality
organization_relationship_quality
```

Issue-code columns:

```text
code
issue_code
decode_issue_codes
```

Object-reference columns:

```text
object_key
source_key
target_key
partition_index
sfs_id
payload_offset
raw_offset
object_offset
object_offset_hex
source_path
source_container
image
```

Columns beginning with `raw_` or ending with `_offset` are diagnostic references
into the source container or object payload.

## Compatibility Artifact Terminology

Public report and JSON schemas use neutral `alternating-byte` terminology for
readable waveform/object payloads that use an alternating filler-byte pattern.
This names the byte pattern and compatibility behavior without exposing internal implementation shorthand. The stable public field families are:

| Surface | Field or value family |
| --- | --- |
| Waveform sidecars and structured export graphs | `alternating_byte_payload_detected`, `alternating_byte_useful_bytes`. |
| Waveform transform status | `alternating-byte-signed-high-byte`, `alternating-byte-compatibility-export`. |
| Object format | `alternating-byte-artifact`. |
| SFS inventory matching | `alternating-byte-object`, `link-id+alternating-byte`, `link-id+alternating-byte-type`. |
| Volume validation counts | `compatibility_artifact_object_entry_count`, `compatibility_artifact_smpl_entry_count`, and object-type-specific `compatibility_artifact_*_entry_count` fields. |
| Volume validation issues | `visible-alternating-byte-compatibility-artifact-objects`, `SFS_VOLUME_VISIBLE_ALTERNATING_BYTE_ARTIFACT`. |

These names are intended to be the public pre-release schema names. Callers
should not rely on older internal names for this payload family.

## Command Outputs

### `axklib info`

`info` writes a sampler-facing tree to stdout. It is intended for navigation and
inspection, not for complete diagnostics. Use CSV/JSON reports for all rows and
raw fields.

Key options:

| Option | Behavior |
| --- | --- |
| `--show-default-programs` | Include default empty Program slots. |
| `--show-unresolved` | Add Unknown placeholders for active missing local Program targets. |
| depth options | Limit rendered tree depth when available. |

### `axklib inventory`

Inventory reports describe container organization and object placement.

Common report files include:

| File | Purpose |
| --- | --- |
| `partitions.csv/json` | SFS partition entries and geometry. |
| `directories.csv/json` | SFS directory records and paths. |
| `directory_entries.csv/json` | Directory entries and target matching. |
| `volumes.csv/json` | Volume-level summaries. |
| `volume_categories.csv/json` | Volume category directories. |
| `volume_objects.csv/json` | Object entries with category placement and match quality. |

Container-specific fields remain in the relevant rows: SFS IDs and object
offsets for SFS, FAT file metadata for floppy objects, and ISO path metadata for
CD-ROM objects.

### `axklib objects`

Object reports decode shared object headers and current object fields.

Important field families:

| Family | Examples |
| --- | --- |
| Shared header | `type`, `known_type`, `header_size`, `payload_bytes_0x1c`, `name_guess`. |
| SMPL metadata | sample rate, sample width, root key, loop fields, compact record bytes. |
| SBNK metadata | member names, link IDs, Program bitmaps, raw sample parameter fields, and resolved sampler-facing key ranges. |
| SBAC metadata | active slot count, slot names, value-enable bitmap fields. |
| PROG metadata | Program display name, assignment rows, common fields, effect blocks. |

### `axklib relationships`

Relationship reports write:

| File | Purpose |
| --- | --- |
| `relationships.csv/json` | Canonical graph edges. |
| `relationship_summary.json` | Counts by quality, relationship type, and row family. |
| detailed SBAC/SBNK rows | Slot-level matching details when emitted by the command. |
| detailed PROG assignment rows | Assignment row matching details when emitted by the command. |
| bitmap rows | SBNK Program-link bitmap comparison rows. |
| schema manifests | Column schemas for generated reports. |

Important relationship fields:

| Field | Meaning |
| --- | --- |
| `relationship_type` | Edge type. |
| `quality` | Relationship quality label. |
| `basis` | Matching method. |
| `assignment_index` | Program assignment slot index. |
| `assignment_name` | Program assignment row name. |
| `assignment_row_state` | Program row decode state. |
| `active_assignment_state` | Active/off/source-load classification. |
| `assignment_rch_assign_display` | Rch Assign display family. |
| `diagnostic_category` | Coarse grouping for diagnostic relationship rows; empty for ordinary graph edges. |

### `axklib coverage`

Coverage reports summarize relationship graph completeness. The summary includes:

| Field | Meaning |
| --- | --- |
| `relationship_count` | Number of graph edges. |
| `known_relationship_count` | Count of `Known` rows. |
| `likely_relationship_count` | Count of `Likely` rows. |
| `tentative_relationship_count` | Count of `Tentative` rows. |
| `unknown_relationship_count` | Count of `Unknown` rows. |
| `ambiguous_relationship_count` | Count returned by `RelationshipGraph.ambiguous()`. |
| `prog_assignment_row_count` | Decoded Program assignment row count. |
| `relationship_type_counts` | Compact counts by relationship type. |

Coverage buckets are diagnostic. They should not be treated as missing active
Program content unless the active-state field says the affected row is active.

SBNK member-link diagnostics keep link/name disagreements separate from Program
assignment diagnostics; CD-ROM cross-folder link/name mismatches use their own
basis value while retaining the shared `sbnk-member-link` category.

### `axklib validate`

Validation reports emit structured issues with stable codes and severity levels.

Important issue fields:

| Field | Meaning |
| --- | --- |
| `code` | Stable validation issue code. |
| `severity` | `error`, `warning`, or another command-defined severity. |
| `message` | User-facing message. |
| `source_path` | Input path. |
| `sampler_path` | Sampler-facing path when available. |
| `object_key` | Related object key when available. |

Content-tree validation currently highlights these relationship issues in
`info` output:

| Code | Meaning |
| --- | --- |
| `REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING` | Active Program path reaches a Sample Bank whose member waveform target is missing. |
| `REL_SBNK_MEMBER_TARGET_MISSING` | Sample Bank member waveform target is missing. |

When a volume has an error-level issue, `info` can append `(errors detected)` to
the affected volume label.

### `axklib extract wav file`

Scoped WAV export writes shared WAV files for the selected object graph. Use
`file` for whole-input extraction, or a narrower scope with one or more `--path`
selectors copied from `info --format paths`.

| Output | Meaning |
| --- | --- |
| `_samples/physical/*.wav` | Exact mono physical waveform exports. |
| `_samples/rendered/*.wav` | Interleaved stereo render when compatible left/right members are identified. |

Rendered stereo is additive. A successful render does not suppress or replace the
physical WAV rows. When several sampler objects reference the same physical
left/right pair in one selection, they can share one rendered WAV path. Stereo
decision rows identify the source objects, mono WAV paths, rendered WAV path,
reason code, relationship quality, and basis.
Known basis values include:

| Basis fragment | Meaning |
| --- | --- |
| `SBNK_LEFT_MEMBER_TO_SMPL` / `SBNK_RIGHT_MEMBER_TO_SMPL` relationship rows | Ordinary single-`SBNK` left/right member render. |
| `same-sbac-sbnk-name-lr-pair` | Paired sibling `SBNK` render under one `SBAC`, using terminal `-L` / `-R` sampler-facing names. |

Stereo decisions use these public reason-code families:
| Reason | Meaning |
| --- | --- |
| `STEREO_EXACT_INTERLEAVED` | Left/right members were written as exact interleaved stereo. |
| `STEREO_PADDED_SHORTER` | Shorter side was padded with trailing zero frames. |
| `STEREO_FRAME_COUNT_MISMATCH` | Frame counts differ and policy kept physical files only. |
| `STEREO_SAMPLE_RATE_MISMATCH` | Sample rates differ. |
| `STEREO_SAMPLE_WIDTH_MISMATCH` | Sample widths differ. |
| `STEREO_MISSING_LEFT_OR_RIGHT` | One side is missing. |
| `STEREO_EXPORT_PARENT_CONFLICT` | Placement conflict prevents one rendered parent path. |
| `STEREO_RELATIONSHIP_NOT_KNOWN` | Relationship quality is not strong enough for rendering. |

## `axklib info --format paths`

The path format prints tab-separated selector rows for copy/paste workflows.
Use the `path` column with targeted extraction commands.

| Column | Meaning |
| --- | --- |
| `source_path` | Input container path. |
| `scope` | Selectable scope: `volume`, `program`, `sbac`, or `sbnk`. |
| `path` | Source-relative selector accepted by targeted `extract wav` and `extract sfz`. |
| `display_name` | Sampler-facing label for the selected node. |
| `object_type` | Object type when the row targets a sampler object. |
| `object_key` | Technical object key for diagnostics and API consumers. |

## `axklib extract sfz file`

`extract sfz <scope>` runs scoped waveform export first, then writes SFZ
instrument files from the selection graphs. Use `file` for whole-input SFZ
export. Use `volume`, `program`, `sbac`, or `sbnk` with repeatable `--path`
selectors for narrower exports.

| Output | Description |
| --- | --- |
| `<instrument>.sfz` | Selection-scoped SFZ instrument file written directly under the selection folder. Each `sample=` path is relative to the containing SFZ file. |

SFZ result rows include the selection path, instrument type, instrument name,
relative SFZ path, rendered and physical region counts, skipped region count,
status, and notes. They are returned by the library API but are not written as
CSV/JSON sidecars by default. The SFZ files prefer rendered stereo WAVs when
present in the graph and fall back to exact physical WAV references otherwise.

Shared WAV files are stored under `_samples/physical/` and
`_samples/rendered/`. Narrow scopes write only the selected dependency closure
instead of a hidden full-input staging export. The scoped loader defers waveform
payload reads until selected objects are decoded; SFZ sample paths remain
relative to the containing SFZ file. Selection graph rows are returned by the
library API and can be written by request-level graph output, but they are not
default CLI sidecars.

## `volume.axklib.json`

`volume.axklib.json` is the per-volume graph manifest shape used by structured
graph exports. Scoped extraction uses arrays of the same graph records in
memory, and API callers may write them as selection graph JSON. Each graph
record contains:

| Section | Meaning |
| --- | --- |
| physical waveform objects | Every decoded `SMPL` object exported as exact mono WAV when possible. |
| sampler objects | `SBNK`, `SBAC`, `PROG`, and related object metadata for the volume. |
| relationships | Relationship graph rows used by export planning. |
| rendered files | Stereo render outputs and padding decisions. |
| diagnostics | Non-render reasons, missing targets, quality labels, and path decisions. |

The graph manifest is the authoritative place to connect WAV files back to
sampler objects. The folder layout is convenient for users; the JSON preserves
object and relationship detail.

Playback rows use Yamaha A-series octave labels: `root_key_name` and
`root_key_name_yamaha` report MIDI note 60 as `C3`, matching the sampler UI.
`root_key_name_scientific` reports the same pitch as `C4`, matching common
scientific-pitch tuners. `root_key_midi` is the convention-independent
authoritative value; the two names differ only by octave numbering and do not
imply transposition.

Physical `SMPL` object rows keep storage-facing names and paths. When a physical
waveform is referenced by sampler-visible `SBNK` members, the row includes
`user_facing_aliases` entries with the member display name, object reference,
optional sample-bank/group owner, and relationship quality. Consumers that want
sampler-facing labels should prefer those aliases over parsing numeric duplicate
suffixes from physical WAV filenames.

Recommended display fallback order for future UI or export-index views, implemented by [`preferred_smpl_display_name()`](graph.md):

1. `user_facing_aliases[0].display_name` when at least one alias is present.
2. The physical `SMPL.display_name` when no sampler-visible alias is known.
3. The physical `SMPL.wav_path` only as a file reference, not as a display label.

`wav_path`, `source_ref`, object IDs, and relationship rows remain authoritative
for exact files and graph references. Alias display names are presentation
metadata and must not be used to rewrite or deduplicate exact WAV paths.

## Public API

::: axklib.reports
    options:
      members:
        - to_plain
        - row_to_dict
        - write_json
        - write_rows_json
        - write_csv
        - write_dict_csv

::: axklib.reports.schema
    options:
      members:
        - ReportColumnSchema
        - ReportSchemaManifest
        - make_schema_manifest
        - write_schema_manifest
        - write_schema_index
