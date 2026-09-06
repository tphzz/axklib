# Program Parameters

`ProgramParameters` contains optional, writable Program-wide settings.
`ProgramAssignmentParameters` contains optional Easy Edit settings for a fresh
or existing assignment. Omission preserves a saved value in an update; explicit
zero, `false`, and `"inherit"` are values, not omissions. JSON rejects `null`,
unknown properties, fractional numbers, overflowing integers, and noncanonical
numbered keys such as `"01"`.

These writes require the current Program layout (selector 4) and an explicit
A4000 or A5000 target model. The model selects writable domains; it does not
convert the Program or remove untouched settings belonging to another model.
Legacy Programs remain readable and preservable but do not accept these
parameter updates. A5000-specific writes have host regression coverage, not a
claim of A5000 hardware validation.

The native `ProgramSpec.parameters` member accepts Program-wide settings;
`ProgramAssignmentSpec.parameters` accepts assignment settings. The same sparse
JSON groups are used by build manifests, Program insertion and parameter updates.
`ProgramSpec.model` defaults to A4000. Omitted fresh settings retain the neutral
template, including inherited assignment receive settings. Authoring profiles
remain as specified in [Writer And Alteration](write.md).

## Fresh Authoring

A Program in a build manifest, or the `program` member of `insert_program`, uses
this shape:

```json
{
  "number": 33,
  "name": "Params",
  "model": "A4000",
  "parameters": {"level": 87, "effects": {"1": {"type": 1}}},
  "assignments": [
    {"sample_bank": "Bank", "parameters": {"receive": {"port": "a", "channel": 1}}},
    {"sample": "Direct", "parameters": {"receive": {"port": "a", "channel": 2}, "pan_offset": 100}}
  ]
}
```

Each row has exactly one target. A fresh assignment's omitted Easy Edit values
use the neutral row template; requested key and velocity limits validate against
those defaults. Unknown properties and obsolete receive-mode fields are rejected.

## Decoded Metadata

`CurrentProg.parameters` and each `ProgAssignment.parameters` use these same
semantic groups. The CLI object JSON and application object-metadata response
place them under `parameters`, retaining the canonical snake-case parameter
names and numbered maps even inside the application's camel-case envelope.
Known inactive values are included. An omitted decoded leaf means its stored
encoding is unavailable or unsupported, not zero, false, or an inferred default.

Read-side domains cover both current models without inferring an authoring
model from the layout selector. A decoded value is not automatic permission to
write it to either model. Validation still applies to the requested patch only.

Raw common and extension blocks, canonical and legacy controller bytes, every
physical effect block and all sixteen unsigned words, and complete assignment
rows remain available beside the typed projection. Unsupported effect types,
action/unused words, out-of-domain encodings, and unnamed bits remain in that raw
storage. The raw receive selector is separate from the semantic receive value.

Legacy selectors 1/2 expose shared stored fields and their three effects and
four controllers. They do not synthesize port-B maps, StepWave, right A/D,
current A/D routing, or effects 4..6. Legacy assignment routing is retained raw,
not presented as current output destinations or level offsets. Controller
functions above the legacy domain likewise remain raw. These omissions do not
reject an otherwise readable object or change byte-preserving operations.

## Atomic Updates

Use `UpdateProgramParametersOperation` in an alteration manifest, or
`update_program_parameters` in the CLI's alteration JSON:

```json
{
  "schema_version": "1.0",
  "operations": [
    {
      "id": "program-settings",
      "type": "update_program_parameters",
      "partition_index": 0,
      "volume_name": "Programs",
      "program_number": 33,
      "model": "A4000",
      "parameters": {
        "level": 87,
        "effects": {"1": {"enabled": false}}
      },
      "assignments": [
        {
          "ordinal": 1,
          "expected_target_kind": "SBNK",
          "expected_target_name": "Direct",
          "parameters": {"pan_offset": 100, "receive": "basic"}
        }
      ]
    }
  ]
}
```

`parameters` and `assignments` are independently optional. The operation must
contain at least one writable leaf, and each supplied assignment patch must
be non-empty. Assignment ordinals are zero-based counted row positions, not
names or visible-list indices. They must be unique, in `0..998`, and within
the Program's current stored count. Both the stored target kind (`SBNK` or
`SBAC`) and name must match. The expected identity is a guard, not a retarget
request. Empty or unsupported rows cannot be patched.

All requested global and assignment edits validate together before replacing
the fixed-size payload. Later operations see the updated state. Invalid values,
stale guards, cancellation, or any later operation failure reject the complete
transaction. The source image is never modified. Retrying an identical patch
is byte-identical, including same-type effects and already-equal settings.

Updates preserve count, capacity, unused rows, assignment targets, opaque bits,
runtime state, and allocation padding. No Program parameter operation changes
Sample Bank propagation state, Sample payloads, or PCM. Unrelated global edits
can preserve unresolved assignments without cleaning or resolving them.

## Program-Wide Groups

Numbered maps are sparse JSON objects. Their C++ equivalents are fixed arrays
of optional leaves or groups. Channel maps use keys `1..16` under `a` and `b`;
the other groups use the ranges below.

| Group | Writable values |
| --- | --- |
| Common | `level` 0..127; `transpose` -127..127 |
| `controller_reset`, `note_toggle` | Boolean channel maps; port `b` requires A5000 |
| `portamento` | `type` 0..3; `rate`, `time` 1..127 |
| `lfo` | `cycle`, `wave` 0..6; `initial_phase` 0..3; `sync` 0..1 (A5000: 0..2); `tempo` 25..250; `sample_hold_speed` 0..127 |
| `lfo` reset | `reset_channel` -2..16 (A5000: -2..32); `reset_note` -1..127; negative values retain their stored special selections |
| `step_wave` | `step_count` 2/3/4/6/8/12/16; `slope` `none`/`rising`/`falling`/`both`; numbered `values` 1..16, each 0..127 |
| `ad` | `enabled` boolean; `source` 0..2; separate `left` and `right` groups |
| A/D channel | `pan` -63..63; `output1` and `output2`, each with `destination` 0..9 (A5000: 0..12) and `level` 0..127 |
| `controllers` 1..4 | `device` 0..126; `function` 0..71 (A5000: 0..128); `type` 0..3; `range` -63..63 |
| `effect_connections` | Connection `1` 0..4; connection `2` 0..4 requires A5000 |
| `effects` | Slots 1..3 on A4000, 1..6 on A5000; fields below |

Controller functions use canonical numeric IDs, not displayed menu indices.
Changing a canonical controller record or A/D output updates only its dependent
legacy projection. Already-equal settings do not normalize unrelated saved
bytes. Latent settings are not erased merely because the current routing makes
them inactive on the sampler UI.

## Effects

An effect accepts `enabled` (boolean), `input_level` and `output_level`
(0..127), `pan` (-63..63), `width` (-126..0), `destination`, `type`, and
numbered `parameters` (physical slots 1..16). Destinations are 0..5; A5000
effects 1..3 additionally allow 6..8.

Ordinary types 0..96 have complete sixteen-word reset vectors and individual
numeric write domains. `effect_write_info()` exposes these domains and the
stored/action/unused classification. Parameter words are unsigned 16-bit values;
they are not universally limited to 127. Action and unused slots cannot be
written independently. Imported type 97 is read/preserve-only, although its
enable flag can be changed without changing its type or words.

A type change resets all sixteen words, including hidden words, then applies
explicit parameter overrides against the new type. Reasserting the same type
does not reset anything. Bypass changes only `enabled`. A fresh explicitly
selected type initializes its reset vector even when it is type 0; omitting
the fresh effect leaves the original neutral template untouched.

## Assignment Easy Edit

`receive` is `"inherit"`, `"basic"`, or an explicit
`{"port":"a","channel":1}` value. Ports are `a` or `b`, channels are 1..16,
and port `b` requires A5000. Inherit means the Sample's receive selection;
Basic means the Basic Receive Channel, not a numeric channel alias.

Most signed offsets have the stored domain -127..127: `level_offset`,
`velocity_sensitivity_offset`, `pan_offset`, `high_velocity_crossfade_offset`,
`low_velocity_crossfade_offset`, `fine_tune_offset`, `coarse_tune_offset`,
`key_shift`, `amp_attack_offset`, `amp_decay_offset`, `amp_release_offset`,
`filter_cutoff_offset`, `filter_cutoff_distance_offset`,
`output1_level_offset`, and `output2_level_offset`. `filter_gain_offset` is
-63..63 and `filter_q_offset` is -31..31. These are stored offsets, not effective
playback values: Pan +100 and -127 are valid even though effective Pan clips.

`key_low`, `key_high`, `velocity_low`, and `velocity_high` are 0..127.
Changed limits validate against the merged result, including omitted saved
counterparts. `output1` and `output2` are replacements: -1 means inherit,
0..9 are A4000 destinations, and A5000 additionally permits 10..12.
`alternate_group` is -1 (inherit) or 0..16. `portamento`, `mono`, and
`key_crossfade` use `"inherit"`, `"off"`, or `"on"`. `midi_control` is boolean.

Changed output replacements or levels update only their dependent legacy
projection. Output 2 is projected first and output 1 wins a shared bucket;
unmatched legacy level bytes remain unchanged. Assignment isolation/Solo flags,
source handles, row identities, and reserved bytes are not authoring parameters.

## Validation Limits

Host regressions cover the supported parameter domains, packed-field isolation,
complete effect initialization, guarded updates, and byte preservation through
HDS, FAT12, ISO9660 and portable Program transfers. Existing sampler checks
cover representative count/tail, effect-update and bank-aware Easy Edit behavior;
they do not constitute an on-device test of every writable parameter combination.
A5000-only settings have host validation, not an A5000 hardware confirmation.

Legacy layouts, raw effect type 97, unused/action effect words and runtime-only
fields remain preservation-only. Parameter support does not relax the entry
point's existing assignment-count or topology limits. Release readiness is a
separate verification gate from this parameter contract.
