# A-Series Program Formats

Programs have distinct A3000 and A4000/A5000 storage formats, independently of
their assigned Sample Banks and Samples. Axkdeck shows `a3k` for Program
header revisions 1/2 and `a4k/a5k` for revision 4. The badge appears in the
Program list and on resolved Multi assignments. **Stored format** in the
inspector includes the revision, counted assignments, stored row capacity and
parameter-extension size.

Unrecognized or malformed Program storage has a `?` badge, unavailable layout
metrics and no conversion command. Distinct Programs with the same numeric
slot remain separate in Single view; Multi shows an ambiguous slot instead
of choosing one arbitrarily.

The badge describes this Program's storage, not the compatibility of its entire
volume, dependencies, output hardware, or System File.

`a3k` does not mean "made on an A3000" or "not usable on an A4000/A5000".
Likewise, `a4k/a5k` does not mean that both models support every stored setting.
A later-model CD-ROM can contain older-format Programs. Loading one on a later
sampler supplies and translates runtime parameters without changing the source
file's stored format. The badge does not summarize which features are used by
the Program's linked Samples or Sample Banks.

A4000/A5000 OS 1.07 and 1.50 save loaded Programs in revision 4, including
Programs loaded from revision 2 and saved without parameter edits. The sampler
does not choose the saved revision according to the features used. See
[Program save revision](sampler-data.md#program-save-revision) for the
distinction between saving from memory and retaining an existing file.

A3000 V2 can also read the legacy-compatible portion of a revision-4 Program.
It ignores the 176-byte extension and uses the older compatibility copies of
effects, controllers and routing. Saving that loaded state produces revision
2 and does not preserve the extension. This is not equivalent to the checked,
lossless downconversion offered by axkdeck, nor a guarantee of identical sound.

## Convert A Program

In Single Program view, right-click one Program and choose **Convert to a3k
program format...** or **Convert to a4k/a5k program format...**. The dialog
checks the Program and explains settings that prevent conversion. Read-only
images can be inspected but not converted. There is no batch or forced-lossy
conversion.

Conversion preserves the numeric Program slot, display name, counted assignment
order, unresolved assignments, unused row capacity and linked objects. It does
not convert Sample Banks, Samples, Wave Data, or SYSTEM/SYSTEM2 files. Multi
assignments remain unchanged. Full Program parameter editing is a separate
feature; conversion does not enable an editor for those parameters.

## Format Differences

| Storage | Revision | Program parameters |
| --- | --- | --- |
| A3000 | 1 or 2 | Three effects, four controllers, native A/D and assignment routing |
| A4000/A5000 | 4 | Adds a 176-byte extension, including effects 4-6, canonical controllers, independent A/D routing, MIDI-B maps and StepWave data |

The later layout is shared by A4000 and A5000; individual settings can still
require A5000. For example, their shared later layout stores six effect blocks,
but only A5000 can use slots 4-6. Physical assignment capacity is not the active count.
Conversion changes neither. See the [storage and model comparison](sampler-data.md#storage-generation-and-sampler-model)
for field locations, length equations, canonical versus compatibility copies,
and per-model feature limits.

Upconversion initializes the extension and translates native settings. A3000
V1 effects that would be replaced by Through on load are refused instead of
silently changing sound. Unexplained bytes that would be overwritten also block
conversion.

For revision 1, reported effect types describe loading on A3000 V2 and later
samplers, not original V1 playback. The CLI and API preserve the stored type
alongside that interpreted value. Original V1 playback is not emulated.

Downconversion writes revision 2 only when all settings can be retained.
Blockers include later-only effect types, effects 4-6 settings, unsupported
controller functions or MIDI-B assignments, StepWave settings, independent A/D
settings and unrepresentable assignment offsets. Inactive settings are not
discarded merely because they are inaudible. Legacy compatibility copies are
regenerated from canonical current values; unrelated opaque data and trailing
bytes remain intact.

Retained native velocity-crossfade bits must agree with the canonical later
offsets; conflicting values block conversion rather than being overwritten.
This preservation check concerns host conversion. It does not promise that
a sampler's subsequent save retains every common or reserved byte.

## Alteration Manifest

An already-matching format is an exact no-op, including revision-1 Programs.
Program format names are `a3000` and `a4000_a5000`, distinct from Sample/Bank
parameter-size format names.

Use `convert_prog_format` in an alteration manifest:

```json
{
  "id": "convert-program",
  "type": "convert_prog_format",
  "partition_index": 0,
  "volume_name": "Programs",
  "program_number": 33,
  "target_format": "a3000",
  "expected_payload_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

Replace the example digest with the SHA-256 of the entire current Program
payload. Slots are 1..128; the numeric slot, not the display name, selects the
Program. A stale hash rejects even a no-op. Both directions run volume-capacity
checks and use the existing atomic alteration transaction. Allocation failure
or a later failed operation does not publish a partial conversion. The desktop
also checks the image revision and offers **Refresh** after a committed write
whose workspace refresh failed, never a second write.

## Validation Scope

Conversion transformations are covered by host tests for raw-byte
preservation, parameter boundaries, guarded transactions and UI recovery.
A paired-control A4000 hardware check exercised both conversion directions
with a representative Program, including saving, reloading into cleared memory
and playback. Parameter spot checks matched, and the returned image retained
the expected Program settings, relationships and exact audio data.

This is bounded coverage, not exhaustive testing of every setting or Program.
Equivalent A3000 and A5000 hardware checks remain outstanding. Program-only
downconversion does not make later-format dependencies A3000-compatible.
Keep backups and test converted media on your intended sampler before relying
on it for a performance.
