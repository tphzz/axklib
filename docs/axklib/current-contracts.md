# Current Contract Index

axklib is pre-release and maintains one current contract. This page maps
cross-cutting subjects to their specification. Format pages describe stored
bytes; operation and output pages describe axklib's supported interfaces and
behavior. Unspecified format semantics remain distinct from unsupported
software features. For format references organized by layer and device family,
start with the [Formats Overview](formats.md).

| Subject | Authoritative current specification |
| --- | --- |
| A-series sampler object bytes and relationships | [A-Series Sampler Object Structures](sampler-data.md) |
| A-series Sample layouts and device-generation differences | [A-Series Sample Formats And Generations](sample-formats.md) |
| A-series SYSTEM/SYSTEM2 configuration layout and unspecified state | [A-Series System Files (SYSTEM / SYSTEM2)](system-files.md) |
| A-series native Sequence timeline | [A-Series Sequence Data (SEQU)](sequences.md) |
| A-series Sequence transfer and MIDI conversion | [A-Series Sequence Transfer And MIDI Conversion](sequence-midi.md) |
| A-series Sample parameter JSON and writable domains | [A-Series Sample Parameter Authoring](sample-parameters.md) |
| A-series Program parameter JSON and writable domains | [A-Series Program Parameters](program-parameters.md) |
| AXK package container, manifest and transfer rules | [AXK Portable Object Packages](portable-packages.md) |
| Writer admission, mandatory planning, and source preservation | [Writer And Alteration](write.md) |
| SFS allocation copies and integrity validation | [SFS Hard Disk Filesystem](sfs-filesystem.md) |
| A3K read-only support and one-volume projection | [A3K Archive Profile](media.md#a3k-archive-profile); external format reference via [A3K Volume Archives](a3k-archive.md) |
| SU700 control, sample and song file structures | [SU700 File Layout And Sample Data](su700.md) |
| SU700 song settings, track configuration, sample scenes and event words | [SU700 Song And Track Records](su700-song.md) |
| SU700 effects scenes, routing and parameter encodings | [SU700 Effects Records](su700-effects.md) |
| Exact physical audio, derived rendered stereo, and output layout | [Names, Paths, And Exports](names-and-paths.md) |
| Relationship-quality admission for exact output | [Names, Paths, And Exports](names-and-paths.md) |
| Separate-output alteration and journaled in-place session mutation | [Writer And Alteration](write.md) |
| Public Program, Sample Bank, Sample, and Wave Data terminology | [Yamaha Glossary](glossary.md) |

## Interpretation

- `--dry-run` uses the same planner as create/apply and must not publish an
  output.
- Exact export preserves decoded physical Wave Data. `--stereo auto` may add a
  rendered stereo WAV; padding a shorter compatible lane changes only that
  derived artifact and is recorded in its graph metadata.
- Program, Sample Bank, and Sample scoped output traverses only `Known`
  dependencies. Other edges remain diagnostics.
- CLI and native alteration write a separately named output. Authenticated
  writable image sessions use a journaled in-place transaction with validation
  and rollback before commit.
- The object names are Program (`PROG`), Sample Bank (`SBAC`), Sample (`SBNK`),
  and Wave Data (`SMPL`). Raw type strings are not expanded into invented
  acronyms.

Historical records and completed work items are not current contracts. They
must be labeled as historical when retained and cannot override the
specifications above.
