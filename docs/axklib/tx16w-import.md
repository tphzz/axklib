# TX16W Disk Import

Axklib can inspect Yamaha-native TX16W floppy images and project their sampler
data into a writable Yamaha A-series SFS volume. This is an import operation,
not a claim that TX16W objects are A-series objects or that a TX16W disk is an
A-series floppy.

Axkdeck accepts one or more `.ima` or `.img` TX16W disks through drag and drop.
The selected files form one logical disk set. When a writable SFS
volume is selected, that volume is the initial destination. When no volume is
selected, the import dialog requires a destination from the open hard-disk
image. FAT12 floppy, ISO9660, archive, and read-only sessions are not valid
destinations because the operation currently authors A-series objects only in
SFS volumes.

The dialog checks native references for missing companion data before preparing
the conversion. A complete explicit selection is authoritative. Otherwise, a
workspace FILE source can search its immediate containing directory on either
a local or remote server. A local desktop file can search authorized sibling
disk images when using the local server. Only uniquely matching suppliers are
added, marked **Auto-added**; overlapping alternatives remain ambiguous. There
is no numbered-filename guess. Use **Add disks** to choose an ambiguous or
missing companion explicitly. Browser files and desktop files uploaded to a
remote server require explicit selection of all members: they do not expose a
server-side source directory.

Source chips retain complete names in hover/focus help and have an adjacent
remove action. Removing a member prevents its automatic rediscovery until it
is explicitly added again. Adding or removing a member invalidates the reviewed
plan and consent immediately, then prepares the complete current set again.
Discovery failures remain visible with Retry; cancellation discards late results
and releases unused candidate uploads. Local path access stays within the
desktop's authorized per-file scope, not a recursive directory grant.

The server also provides the read-only
`POST /api/v1/companion-source-resolutions` job
(`images.companionSources.resolve`). It resolves selected FILE or UPLOAD sources
and returns actual members with their `SELECTED` or `AUTO_ADDED` origin and
remaining missing, ambiguous, unsupported or invalid requirements. A complete
explicit selection is authoritative and does not trigger discovery. Otherwise,
workspace FILE sources may search only the immediate parent of the first
selected file, including on remote servers. Discovery uses native references,
not numbered-filename guesses, and adds only uniquely matching suppliers.
Overlapping suppliers remain ambiguous rather than being selected by order.

Discovery is bounded to 1,024 directory entries, 4 MiB per source and 64 MiB of
candidate input, with at most 32 accepted members. It does not recurse or follow
links. Limits and cancellation fail explicitly. Exclusions suppress automatic
rediscovery; explicit re-addition takes precedence. UPLOAD references never
trigger a search of the server's staging directory. The dialog invokes this
resolution job automatically and reuses completed membership when only the
destination or fidelity changes.

The import uses the shared [directional conversion plan](conversion.md) contract.
Alternatively, open a TX16W document first and use **Convert** to select an
A-series destination image on the same server. Both paths prepare one immutable
plan and execute its reviewed output without repeating conversion.

## Import Stages

Inspection decodes TX16W Waves, Timbres, Voices, Performances, key regions,
assignments, native controls, and relationships. Destination planning then maps
that content to A-series Programs (`PROG`), Sample Banks (`SBAC`), Samples
(`SBNK`), and Wave Data (`SMPL`). An inspectable source is not necessarily
fully representable on the destination: review conversion notices before import.

## Supported Source Profile

The current profile recognizes Yamaha-native TX16W sets and their associated
Wave files in a FAT12 disk image:

| Source file | Decoded role |
| --- | --- |
| `*.Wnn` | Packed signed 12-bit PCM Wave, rate, attack length, repeat length, and loop mode |
| `*.Snn` | Setup table and Wave references |
| `*.Vnn` | Voice definitions and key regions |
| `*.Unn` | Performance definitions and Voice assignments |

A native set requires matching Setup and Voice files. Both the legacy Setup
layout and the `0200` layout are recognized. Independent native Setup groups
remain separate rather than being merged by shared Wave names. A Performance file is
optional; when it is absent, the adapter can create audition Programs for the
decoded Voices. Companion `P`, `O`, `C`, `R`, and `X` files are recognized as
auxiliary data but are not decoded by this profile. In particular, `.Cnn` Wave
payloads are not silently treated as absent `.Wnn` files. Compressed Wave data
is outside the current Yamaha-native profile and remains visibly unsupported
rather than being guessed.

A Voice may reference a populated Timbre whose native name is blank. The reader
keeps that record and its exact file-local slot; it does not create objects for
unused blank slots or resolve references to empty records. A fallback display
label does not change the native name.

The parser is bounded and rejects malformed headers and truncated logical data.
Wave decoding uses the attack-plus-repeat frame count declared by the TX16W
header; unused bytes at the end of a FAT allocation are not interpreted as PCM.
An odd final frame is decoded without inventing its unused packed partner.
Unknown native sample-rate markers use a visible 33,333 Hz default with a
verification notice rather than silently claiming an exact decode.

References to Waves that are not present in the selected disk set are reported
as blocking mappings in hierarchy mode. A companion disk can satisfy a
reference made by a Setup on another selected member. If the set remains
incomplete, **Wave Data only** is an explicit recovery mode: it imports every
decodable `.Wnn` Wave from the selected members without inventing Programs,
Sample Banks, or Samples. Hierarchy mode imports only Waves reached by a valid
TX16W hierarchy; it does not retain otherwise unreferenced source Waves.

## A-Series Projection

The adapter uses the following hierarchy:

| TX16W source | A-series destination |
| --- | --- |
| Performance | Program (`PROG`) |
| Voice | Sample Bank (`SBAC`) |
| Voice region and referenced Timbre | Sample (`SBNK`) |
| Wave | Wave Data (`SMPL`) |

Program slots are allocated from the first available slots in the destination.
Names are normalized to the destination object limits and disambiguated against
existing objects. The preview shows the final slots, names, relationships, key
ranges, and target sample rates before the image is changed.

Import is one transactional alteration. A blocked mapping prevents the entire
operation, so a partial TX16W hierarchy is not left in the destination volume.

## Parameter Mapping

The preview classifies every non-trivial mapping:

- **Exact** means the source value has a direct supported destination value.
- **Representation** means an equivalent encoding changes, such as expanding
  signed 12-bit PCM to 16-bit PCM or translating native key numbering.
- **Loss** means resampling, approximation or an omitted source control.
- **Uncertainty** means an unverified interpretation or destination default.
- **Blocker** means the current writer profile cannot preserve the relationship
  safely, so import is disabled.

**Best** permits explicitly acknowledged loss and uncertainty. **Strict** rejects
both, while allowing exact and representation-only changes. Unsupported or
ambiguous relationships cannot be acknowledged away.

In Best mode, the fixed **Accept all losses** control covers LOSS items across
every report page. It does not accept UNCERTAINTY items; those require individual
review. A mixed checkbox accepts all losses, and a checked checkbox clears only
loss consent. The control is disabled during preparation or a write/recovery
lock, for Strict fidelity, and when the report has no losses. Preparing a new
plan clears previous consent.

Current parameter behavior is:

| TX16W parameter | A-series behavior |
| --- | --- |
| Packed 12-bit PCM | Decoded to signed PCM and authored as Wave Data |
| Native sample rate | Preserved when supported; otherwise resampled to the nearest supported A-series rate |
| Attack and repeat lengths | For a looped Wave, attack becomes the loop start and the remaining resampled frames become the loop length |
| Loop mode | A native loop becomes an A-series forward loop; a non-looped Wave becomes one-shot playback with no loop window |
| Timbre root key | Translated from the TX16W native key numbering by `-16` |
| Voice region low/high keys | Translated to A-series Sample key ranges |
| Performance receive channel | Mapped to `=Smp` or a one-based A-channel (`A01` through `A16`) |
| Voice Fade | Reported but not authored |
| Per-Voice volume, detune, transpose, output, and alternative group | Reported but not authored |
| Destination-only Sample fields | Use the existing A-series writer defaults |

The A-series adapter preserves the TX16W Performance capacity of 16 Voice
assignments. A Voice referenced by several Performances is authored once as a
Sample Bank and shared by the resulting Programs. If the destination has no
free Program slot, the affected mapping is blocked. These constraints belong
to the A-series adapter, not to the neutral TX16W parser.

## Validation Workflow

Use the import dialog as a plan review:

1. Drop the selected `.ima` or `.img` files onto axkdeck. For a multi-floppy
   source, review automatically found companions or select known members
   together. Add missing or ambiguous members in the dialog.
2. Select a writable SFS volume if no destination was already selected.
3. Choose **Programs and samples** for a complete hierarchy. Use **Wave Data
   only** deliberately when recovering decodable Waves from an incomplete or
   partly unsupported set.
4. Review the paged action table: final names, slots, assignments, key ranges,
   sample rates and loop boundaries.
5. Select Best or Strict and review the mapping issues. In Best mode, explicitly
   acknowledge losses individually or with **Accept all losses**; acknowledge
   each uncertainty individually before continuing.
6. Import only when the retained plan reports that it is ready. Changes to inputs
   or destination require preparation again; expired or consumed plans cannot be
   executed. Use Refresh after a saved import whose cleanup or refresh failed,
   and Check status after an uncertain response. Never repeat an uncertain write.

For audible validation, compare the imported A-series Program layout with the
same TX16W disk in a known TX16W implementation. Check Program assignments,
key boundaries, root keys, pitch, loop points, and loop mode separately. An
audible match does not promote an omitted source control to an exact mapping.
