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

Each region uses the active Sample (`SBNK`) member playback window. The SFZ
`offset` and `end` opcodes restrict the physical Wave Data WAV, while loop mode
and loop positions come from the Sample rather than from the underlying Wave
Data object. A shared Wave Data object can therefore back several Samples with
different exact windows. Forward one-shot and forward continuous-loop modes
are supported. Reverse, bidirectional, invalid, or out-of-range Sample windows
fail the export; they are never approximated with physical Wave Data loop
metadata.

Confirmed stereo members use one rendered stereo WAV only when both member
windows and loop policies match. Otherwise the SFZ keeps the confirmed mono
lanes and applies each lane's own window. Physical mono WAVs remain exact.
