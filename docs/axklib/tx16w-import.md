# TX16W Disk Import

Axklib can inspect Yamaha-native TX16W floppy images and project their sampler
data into a writable Yamaha A-series SFS volume. This is an import operation,
not a claim that TX16W objects are A-series objects or that a TX16W disk is an
A-series floppy.

Axkdeck accepts one or more `.ima` or `.img` TX16W disks through drag and drop.
The selected files form one explicit logical disk set. When a writable SFS
volume is selected, that volume is the initial destination. When no volume is
selected, the import dialog requires a destination from the open hard-disk
image. FAT12 floppy, ISO9660, archive, and read-only sessions are not valid
destinations because the operation currently authors A-series objects only in
SFS volumes.

The dialog does not guess companion filenames or scan the containing directory.
Select all members together, or use **Add disks** before importing. This avoids
mixing alternate editions that happen to have similar names. Adding or removing
a member reruns inspection and the destination plan for the complete selected
set.

## Architecture

TX16W support has two independent layers:

1. `axklib/tx16w.hpp` defines a neutral TX16W data model and parser. It decodes
   TX16W Waves, Timbres, Voices, Performances, key regions, assignments, native
   controls, and relationships without depending on A-series object types.
2. `axklib/tx16w_a_series.hpp` is an adapter. It projects a decoded TX16W
   inspection into an A-series import plan containing Programs (`PROG`), Sample
   Banks (`SBAC`), Samples (`SBNK`), and Wave Data (`SMPL`).

The separation is intentional. Applications that target a different sampler
family can reuse the TX16W parser and data model while supplying a different
adapter. A target adapter should not be added to the neutral parser.

The TX16W headers are source-level library interfaces. They are not currently
part of the installed high-level SDK facade described in [C++ API](cpp-api.md).

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
layout and the `0200` layout are recognized, and several independent native
Setup groups may be combined in one selected disk set. A Performance file is
optional; when it is absent, the adapter can create audition Programs for the
decoded Voices. Companion `P`, `O`, `C`, `R`, and `X` files are recognized as
auxiliary data but are not decoded by this profile. In particular, `.Cnn` Wave
payloads are not silently treated as absent `.Wnn` files. Compressed Wave data
is outside the current Yamaha-native profile and remains visibly unsupported
rather than being guessed.

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
- **Approximated** means the value is translated or resampled according to a
  documented rule.
- **Defaulted** means the destination needs a value that the source does not
  supply.
- **Not imported** means the source field remains visible in the preview but is
  not authored.
- **Blocked** means the current writer profile cannot preserve the relationship
  safely, so import is disabled.

Current parameter behavior is:

| TX16W parameter | A-series behavior |
| --- | --- |
| Packed 12-bit PCM | Decoded to signed PCM and authored as Wave Data |
| Native sample rate | Preserved when supported; otherwise resampled to the nearest supported A-series rate |
| Attack and repeat lengths | For a looped Wave, attack becomes the loop start and the remaining resampled frames become the loop length |
| Loop mode | A native loop becomes an A-series forward loop; a non-looped Wave becomes one-shot playback with no loop window |
| Timbre root key | Translated from the TX16W native key numbering by `-16` |
| Voice region low/high keys | Translated to A-series Sample key ranges |
| Performance receive channel | Mapped to `=SMP` or a one-based MIDI channel |
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
   source, select every known member together or add the remaining members in
   the dialog.
2. Select a writable SFS volume if no destination was already selected.
3. Choose **Programs and samples** for a complete hierarchy. Use **Wave Data
   only** deliberately when recovering decodable Waves from an incomplete or
   partly unsupported set.
4. Review the object counts and expand the Program, Sample Bank, Sample, and
   Wave Data sections.
5. Review all approximated, defaulted, omitted, and blocked mappings.
6. Import only when the preview reports that it is ready.

For audible validation, compare the imported A-series Program layout with the
same TX16W disk in a known TX16W implementation. Check Program assignments,
key boundaries, root keys, pitch, loop points, and loop mode separately. An
audible match does not promote an omitted source control to an exact mapping.
