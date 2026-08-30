# SFZ Export

SFZ export uses decoded Sample ranges and exact Wave Data files. Whole-input
and selected Program exports are supported.

```bash
axklib extract sfz file source.hds --output-dir exports/sfz
axklib info source.hds --format paths
axklib extract sfz program source.hds \
  --path "partition_00/New Volume/Programs/001: Example" \
  --output-dir exports/program
```

Only `Known` relationships are rendered as regions. Program assignments,
Sample Bank membership, and Sample-to-Wave-Data membership must all remain
confirmed through the complete selected dependency closure. Unconfirmed rows
remain diagnostics instead of being assigned to arbitrary Wave Data.

Interactive/session export may still produce the confirmed SFZ subset when at
least one instrument has a complete `Known` dependency path and every remaining
issue is nonfatal. Unconfirmed Program assignments are omitted and reported as
warnings; they are not guessed from names or local context. A fatal issue, or a
selection with no confirmed instrument, makes SFZ unavailable while preserving
WAV-only export for resolved Wave Data.

Each region uses the active Sample (`SBNK`) member playback window. The SFZ
`offset` and `end` opcodes restrict the physical Wave Data WAV, while loop mode
and loop positions come from the Sample rather than from the underlying Wave
Data object. A shared Wave Data object can therefore back several Samples with
different exact windows. Forward one-shot and forward continuous-loop modes
are supported. A-series release-tail looping preserves its exact loop bounds
as `loop_continuous` and reports that SFZ cannot preserve the release-tail
transition. Reverse, bidirectional, invalid, or out-of-range Sample windows
fail the export; they are never approximated with physical Wave Data loop
metadata.

The physical WAV pool is shared across Samples, so each pooled WAV can contain
only Wave Data-level `smpl` and `inst` metadata. Per-Sample root, fine tune,
coarse transpose, key and velocity ranges, playback window, and loop policy
remain authoritative in the SFZ region. A separately rendered stereo Sample
receives Sample-level WAV metadata only when both members agree.

Confirmed stereo members use one rendered stereo WAV only when both member
windows and loop policies match. Otherwise the SFZ keeps the confirmed mono
lanes and applies each lane's own window. Physical mono WAVs remain exact.

One export operation owns one content-addressed `_samples` pool. Wave Data
shared by Samples in different selected volumes is therefore emitted once and
referenced from every volume view. The pool is selection-wide, not
volume-local.

Flat media with one logical volume, including Yamaha FAT12 floppies and sampler
object directories, does not add synthetic `objects/FAT root` or
`objects/Object directory` levels. Its SFZ files and graph sit directly below
the chosen export directory beside the shared `_samples` pool. SFS/HDS and
ISO9660 exports retain their real partition/group and volume hierarchy.
