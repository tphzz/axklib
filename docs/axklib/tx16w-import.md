# TX16W Disk Import

Axklib can inspect Yamaha-native TX16W floppy images and project their sampler
data into a writable Yamaha A-series SFS volume. This is an import operation,
not a claim that TX16W objects are A-series objects or that a TX16W disk is an
A-series floppy.

## Inspect without importing

Use `info --source-set` when the paths are explicit members of one TX16W disk
set. This read-only operation needs neither a running server nor a destination.
Plain `info disk-a.img disk-b.img` still inspects two independent containers.

```sh
axklib info --source-set disk-a.img disk-b.img
axklib info --source-set disk-a.img disk-b.img --document tx16w-source-1 --collection objects --offset 0 --limit 100
```

Use the document ID from the first response. Select `relationships` to see
resolved, missing or ambiguous references. Each object's locations identify
the member, filesystem root, entry and record range. Select by ID rather than
by name: two disks may contain the same filename or identically named presets.
An incomplete set can be browsed; supply the missing disk explicitly to
inspect the combined set.

In the installed C++17 SDK, include `<axklib/sdk.hpp>` and
`<axklib/sdk/source_set.hpp>`, then call
`axk::source_set::open(paths, context)`. Its `summary_json()`,
`objects_json(documentId, offset, limit, context)` and
`relationships_json(documentId, offset, limit, context)` methods read the
catalog from that open. Limits are 1 to 1,024 records per page. Opening a set
checks its input files; paging the retained catalog does not read audio or
rediscover the disks. This file-client mode currently recognizes Yamaha-native
TX16W records on FAT images and A-series Wave Data on regular SFS images.
To go in the opposite direction, see [Create a TX16W Wave disk](conversion.md#create-a-tx16w-wave-disk).
This file-client mode does not search neighboring paths, open raw
devices or grant permission to write to the input.

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
disk images when using the local server. The search starts with the selected
set's missing native files. A newly found Setup can name additional Waves; the
search follows those references in the same directory. Nearby names in natural
sort order are considered first, but a filename cannot establish membership.
Only nonconflicting suppliers needed by the selected set are **Auto-added**.
Use **Add disks** to choose an ambiguous or
missing companion explicitly. Browser files and desktop files uploaded to a
remote server require explicit selection of all members: they do not expose a
server-side source directory.

For an already-open disk set, **Search nearby images and retry** uses the same
matching rules. If multiple disks supply the named Wave, the open set stays
unchanged. Use **Add floppy image** to attach the known companion. Once the
set is complete, no nearby search is needed.

Source chips retain complete names in hover/focus help and have an adjacent
remove action. Removing a member prevents its automatic rediscovery until it
is explicitly added again. Adding or removing a member invalidates the reviewed
plan and consent immediately, then prepares the complete current set again.
Discovery failures remain visible with Retry; cancellation discards late results
and releases uploads owned by the canceled operation. A local desktop drop
reuses the seed upload from floppy inspection. Local path access stays within the
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
candidate input, with at most 32 accepted members. It follows references, not
subdirectories or filesystem links. On a local desktop drop, sibling candidates
are inspected locally; only selected and accepted disks are uploaded. Limits
and cancellation fail explicitly. Exclusions suppress automatic
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
label does not change the native name. Each Voice file selects its own layout:
legacy Voices address 64 Timbres and version-0200 Voices address 128.

The parser is bounded and rejects malformed headers and truncated logical data.
Wave decoding uses the attack-plus-repeat frame count declared by the TX16W
header; unused bytes at the end of a FAT allocation are not interpreted as PCM.
An odd final frame is decoded without inventing its unused packed partner.
Unknown native sample-rate markers use a visible 33,333 Hz default with a
verification notice rather than silently claiming an exact decode.

In hierarchy mode, **All sources in disk set** selects every Performance, or
every Voice in a group with no Performances. The planner follows their Voice
regions and Timbres to the required Waves. An unused Timbre remains browsable
with its missing reference, but cannot block an unrelated sound. A selected
Timbre that needs a missing named Wave blocks both Best and Strict. Adding its
companion can supply the named Wave. A confirmed empty Setup slot has no file
to find: Best omits just that Voice region and lists its name, native key
range and Wave slot under Review. Surviving regions keep their limits. If every
region of a selected sound is empty, Best names the omitted sound and imports
the others. An entirely empty selection writes nothing. Strict rejects these
omissions. Ambiguous, malformed or unsupported dependencies remain blockers.
If the set remains
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
  signed 12-bit PCM to 16-bit PCM.
- **Loss** means resampling, approximation or an omitted source control.
- **Uncertainty** means an unverified interpretation or destination default.
- **Blocker** means the current writer profile cannot preserve the relationship
  safely, so import is disabled.

**Best** reports known changes, such as resampling a Wave, without requiring
acceptance. Only an explicit mapping decision needs confirmation. For example,
when a Voice has no Performance, review whether to create an audition Program
for it. Preparing a new plan clears the previous choices.

**Strict** rejects loss and uncertainty, while allowing exact and
representation-only changes. A blocker cannot be waived in either mode. If a
selected Performance has a broken dependency, exclude that Performance or
correct its source before converting the hierarchy. Review lists each broken
relationship once and identifies a selected sound that needs it when one is
known. To recover just the readable audio, choose **Wave Data only**; this
does not reconstruct missing Voice assignments.

Current parameter behavior is:

| TX16W parameter | A-series behavior |
| --- | --- |
| Packed 12-bit PCM | Decoded to signed PCM and authored as Wave Data |
| Native sample rate | Preserved when supported; otherwise resampled to the nearest supported A-series rate |
| Attack and repeat lengths | For a looped Wave, attack becomes the loop start and the remaining resampled frames become the loop length |
| Loop mode | A native loop becomes an A-series forward loop; a non-looped Wave becomes one-shot playback with no loop window |
| Original pitch | Legacy files use the Timbre root; version-0200 files carry a separate original pitch per Voice region. Each Sample keeps its own root. Shared Wave Data keeps its first translated root and reports differing roots. Fine-pitched words that cannot be mapped to a whole note are reported and the region is omitted. |
| Voice region low/high keys | Packed split limits convert to inclusive MIDI notes 13 through 108. Omitted regions do not extend their neighbors. Out-of-range or invalid splits are reported, not clamped. |
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

From an open TX16W disk set, right-click **/** in Device view and choose
**Convert...**. Choose **Browse...** for the destination image, then select its
partition. Choose **Volume: Existing** to add the planned objects to a volume,
or **Volume: New** to create a named volume in that partition. The volume and
its objects are written together; canceling a preview creates nothing. Neither
choice replaces the image. This workflow
does not create a new image or format a physical disk.

Use the import dialog as a plan review:

1. Drop the selected `.ima` or `.img` files onto axkdeck. For a multi-floppy
   source, review automatically found companions or select known members
   together. Add missing or ambiguous members in the dialog.
2. Choose a partition on a writable SFS image, then an existing or new volume.
3. Choose **Programs and samples** for a complete hierarchy. Use **Wave Data
   only** deliberately when recovering decodable Waves from an incomplete or
   partly unsupported set.
4. Select the source occurrences to convert. Hierarchy selection includes
   required dependent objects; Wave Data only allows individual Waves.
   Review the planned names, slots, assignments, key ranges, rates and loops.
5. Choose Best or Strict and review the conversion notes. In Best mode,
   confirm only the specific mapping choices requested by the plan.
6. Import only when the retained plan reports that it is ready. Changes to inputs
   or destination require preparation again; expired or consumed plans cannot be
   executed. Use Refresh after a saved import whose cleanup or refresh failed,
   and Check status after an uncertain response. Never repeat an uncertain write.

For audible validation, compare the imported A-series Program layout with the
same TX16W disk in a known TX16W implementation. Check Program assignments,
key boundaries, root keys, pitch, loop points, and loop mode separately. An
audible match does not promote an omitted source control to an exact mapping.
