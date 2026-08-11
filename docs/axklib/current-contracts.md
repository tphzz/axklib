# Current Contract Index

axklib is pre-release and maintains one current contract. This page maps
cross-cutting subjects to their authoritative specification; it does not create
parallel rules. When documents disagree, update the named specification and
every in-repository producer, consumer, test, and summary together.

| Subject | Authoritative current specification |
| --- | --- |
| Writer admission, mandatory planning, and source preservation | [Writer And Alteration](write.md) |
| Read-only A3K archive envelope and one-volume projection | [A3K Volume Archives](a3k-archive.md) |
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
