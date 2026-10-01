# Volume Load Capacity

The capacity check answers whether an A-Series SFS volume fits the selected
sampler's parameter memory and shared object slots after uninterrupted power-on
initialization without boot auto-load, a completed Wipe, then full VOLUME/LOAD.
It does not measure PCM RAM, disk space, playback fidelity, or a merge into
existing sampler memory. Wipe retains Programs, so an EEPROM-configured boot
auto-load or earlier RAM edits do not establish this baseline.

| Load Target | Parameter Pool | Shared Slots | Clean Baseline |
| --- | ---: | ---: | ---: |
| A3000 V2 | 512 KiB | 1,024 | 87,720 bytes / 129 slots |
| A4000/A5000 | 768 KiB | 2,048 | 111,280 bytes / 130 slots |

Slots are shared by Samples, Banks, Programs, Wave metadata and Sequences; they
are not a fixed maximum Sample count. Large Sequences can exhaust parameter
memory with few Samples. Physical unused assignment rows and temporary
replacement allocations also matter.

## Results

- `FITS`: the native load replay's exact peak fits both pools. This is a capacity
  result, not a guarantee of compatible playback.
- `DOES_NOT_FIT`: exact replay or a proven minimum exceeds a limit. Import is blocked.

Replay follows native allocations and descriptor order, including temporary
membership copies and cleanup, mixed target selectors, and stored object names.
A Sample with missing Wave Data follows the native known-skip path and reports a
warning; a fitting result does not make that Sample playable. Malformed or
unsupported object structures are errors, not capacity statuses. A3000 V1,
concurrent CD-R authoring and retained-memory merge loads are outside this check.

## Desktop

Hover or focus a volume to see both target profiles. The volume inspector shows
the same read-only A3000 and A4000/A5000 comparison under **Sampler Capacity**.
This panel opens automatically only when either profile does not fit; otherwise
it starts collapsed. Manual expansion choices apply to the current volume revision.
Each profile lists parameter memory and shared object slots. Memory amounts use
KiB and show the exact maximum required during loading when available, or a
proven lower bound marked **&ge;**. Focus or hover a metric label for exact bytes
and the clean-load assumptions.

Import dialogs offer **Sampler load
target**, initially selected from **Preferred A-Series generation**. This is
independent of the imported Sample/Bank storage format and can be changed for
each operation. It is not stored as a property of the volume.

Imports check the prepared final volume, including existing objects and all
batch additions. Fitting checks permit writing; capacity failures block writing.
Unsupported or malformed loads display the normal inspection error state.
Oversized volumes remain readable; deletion, ordinary renaming and proven
nonincreasing Sequence-only replacements remain available. Skipped existing
Files imports do not require capacity admission.
Files imports outside sampler volume categories are unchanged.

## CLI

```bash
axklib capacity volume image.hds --partition 0 --volume "My Volume" --pretty
```

Partition indices are zero-based. The report includes both profiles, exact or
minimum metrics, object counts and refusal reasons. Inspection reads metadata,
not waveform PCM. A duplicated volume name cannot be selected ambiguously.

## API And SDK

`inspect_volume_capacity` identifies a volume by its physical directory record.
The session API `images.volume_capacity.inspect` uses an image revision and
volume scope; stale revisions are rejected. The report is revision-cached.

Mutation preflight and writing share the same prepared edits. A capacity policy
contains only `target` (`A3000` or `A4000_A5000`).
The default for callers that omit it is `A4000_A5000`. Preflight returns
`capacity` with `target`, `reports` and `allowed`.
Package and floppy writes apply an owner-bound retained `planToken`.
Alteration and Files writes pass the policy through their request.

Program generation and placement repair expose read-only preparation operations
that return a canonical session alteration request. The desktop reviews that
request and submits it through the same alteration guard. Preparation grants no
write authority. The SDK also offers `inspect_hds_build_capacity`,
`inspect_hds_alteration`, `inspect_package_import_capacity` and
`inspect_sfs_file_edit_capacity` before the corresponding write.

Retained plans preserve source identity, selected target, owner and current
revision. A changed input or target requires new planning. Known overflow,
invalid structure, cancellation and I/O errors cannot
be turned into successful admission. Immutable candidate data is checked again
before publication or the first journaled write; a rejection leaves the source
unchanged. Capacity admission does not weaken object validation.
