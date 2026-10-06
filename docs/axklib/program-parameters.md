# A-Series Program Parameters

An A-series Program's `parameters` JSON object contains optional, writable Program-wide
settings. Each assignment's `parameters` object contains optional Easy Edit
settings for a fresh or existing assignment. Omission preserves a saved value in an update; explicit
zero, `false`, and `"inherit"` are values, not omissions. JSON rejects `null`,
unknown properties, fractional numbers, overflowing integers, and noncanonical
numbered keys such as `"01"`.

Updates require revision 2 with an explicit A3000 model, or revision 4 with an
explicit A4000 or A5000 model. The model selects writable domains; it does not
convert the Program or remove untouched settings belonging to another model.
Revision-1 Programs remain readable and preservable but do not accept parameter
updates. Explicit [Program format conversion](program-formats.md)
is a separate guarded operation. Model-specific compatibility is bounded as described under
[Validation Limits](#validation-limits).

The same sparse JSON groups are used by build manifests, Program insertion
and parameter updates. A fresh Program's `model` defaults to A4000. Omitted fresh settings retain the neutral
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

The CLI object JSON and application object-metadata response use these same
semantic groups for Programs and assignments under `parameters`, retaining the canonical snake-case parameter
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
current A/D routing, or effects 4..6. Native routing is exposed in its own
domains: output 1 is -1..4, output 2 is -1..5 for assignments (A/D excludes -1).
Native output levels and the inherited/off/on velocity-crossfade switch are
also exposed. Native rows do not expose the current velocity-sensitivity or
separate low/high velocity-crossfade offsets. Controller
functions above the legacy domain likewise remain raw. These omissions do not
reject an otherwise readable object or change byte-preserving operations.

## Atomic Updates

Use `update_program_parameters` in the CLI's alteration JSON:

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

An optional `expected_payload_sha256` is a 64-character lowercase SHA-256 digest
of the inspected logical Program payload. A mismatch rejects the operation
before editing. The desktop editor supplies this guard together with the image
revision, numeric Program slot and assignment identities on every save.

All requested global and assignment edits validate together before replacing
the fixed-size payload. Later operations see the updated state. Invalid values,
stale guards, cancellation, or any later operation failure reject the complete
transaction. The source image is never modified. Retrying an identical patch
is byte-identical, including already-equal settings. An explicitly requested
effect reset reapplies its complete initialization vector.

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

For native A3000 Programs, the shared groups use the same domains except:
LFO wave is 0..5, controller device is 0..125 and function is 0..63. There is
no StepWave, port B, independent right A/D route or second effect-connection
group. Native A/D destinations are output 1: 0..4 and output 2: 0..5. Only
effect slots 1..3 and ordinary effect types 0..54 are writable. Native writes
address the native lanes directly and preserve current-only/opaque bytes.

Controller functions use canonical numeric IDs, not displayed menu indices.
Changing a canonical controller record or A/D output updates only its dependent
legacy projection. Already-equal settings do not normalize unrelated saved
bytes. Latent settings are not erased merely because the current routing makes
them inactive on the sampler UI.

## Effects

An effect accepts `enabled` (boolean), `input_level` and `output_level`
(0..127), `pan` (-63..63), `width` (-126..0), `destination`, `type`, `reset_parameters`, and
numbered `parameters` (physical slots 1..16). Destinations are 0..5; A5000
effects 1..3 additionally allow 6..8.

Ordinary types 0..96 have complete sixteen-word reset vectors and individual
numeric write domains. Slots are classified as stored parameters, actions, or
unused words. Parameter words are unsigned 16-bit values;
they are not universally limited to 127. Action and unused slots cannot be
written independently. Imported type 97 is read/preserve-only, although its
enable flag can be changed without changing its type or words.

A type change resets all sixteen words, including hidden words, then applies
explicit parameter overrides against the new type. Reasserting the same type
does not reset anything unless `reset_parameters: true` is supplied with an
explicit `type`. This write intent permits a reviewed A-to-B-to-A selection
to retain its final reset even though the type matches the original. Native
types use their native sixteen-word defaults, including hidden tails.
Bypass changes only `enabled`. A fresh explicitly
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
Native revision-2 assignments additionally accept `velocity_crossfade` with
the same three selections, instead of the current layout's velocity offsets.

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
On A5000 system software 1.50, hardware compatibility covers the Effect 1-to-Effect 4
Hall route, Effect 4 Dry/Wet and Pan controller modulation, Effect 4 bypass,
Program-local edits, MIDI IN-A/IN-B receive isolation, and save/reload persistence.
This does not establish every Effect 5/6 routing topology, every DSP algorithm,
or every freshly authored parameter combination. Sample output destinations
must be selected for the correct output lane; Output 1 value `2` selects Ef1,
whereas value `7` selects AssgnOut3&4.

Revision-1 layouts, raw effect type 97, unused/action effect words and runtime-only
fields remain preservation-only. Parameter support does not relax the entry
point's existing assignment-count or topology limits. Release readiness is a
separate verification gate from this parameter contract.

## Desktop Editor

The lower zone exposes Sample Select, Easy Edit, Effects, Setup and Control.
Edits form a draft with gesture-based undo/redo, Discard and atomic Save.
Sample Select edits membership and receive channels directly in its table.
Turn off Show only assigned to expose available Samples and Sample Banks.
Removing a row with non-default Easy Edit settings requires confirmation.
Membership and parameter edits share one undo history and one atomic Save.
Duplicate stored rows remain independent, including after another row is removed.

Easy Edit is per assigned Sample or Sample Bank, not a global Program modifier.
Its Sample/Bank selector follows the assignment table and mapping-block
selection; Program-wide Effects, Setup and Control pages do not have that
selector. A Sample Bank's
active parameter overrides are applied when calculating member previews.
Amp EG shows the source and effective curves and edits the three rate offsets;
levels remain read-only in this view. Time spacing is relative, not calibrated
milliseconds. A bank member selector chooses the envelope being previewed.

Key/Velocity shows compact keyboard ranges. Mapping Editor opens the full view
in a separate desktop window that follows the main Program selection. Both
windows share the selected assignment, draft, undo history, Save and Discard;
closing the Mapping Editor does not discard accepted edits. Source outlines,
effective coverage, Program limits and root keys are distinct. Drag or
keyboard-adjust the Program limits, or drag a mapping block to move its limits.
A movement begins after a small pointer threshold, preserves the range's size,
and clamps to the key/velocity boundaries. Release accepts one undo step; Escape
or pointer cancellation accepts nothing. Moving a block does not move its root.
Key shift is a separate parameter. Limits intersect the
shifted Sample range and cannot expand its playable coverage. Bank members
remain separately visible; unresolved sources do not acquire invented ranges.
The compact view places low/high-key handles directly on the keyboard. Both
views label every C key vertically, centered on its white key. All resolved
Samples alternate between mint green and a shade 10% darker in source-range
order. The shades distinguish neighboring mappings, not permanent Sample
identities; selection, panning and zooming do not change their order. Overlapping
coverage has subtle hatching and contributor tooltips.
The selected Program limits have a separate outline, including keys with no
playable source. Key fields also show note names, and the shared velocity editor
adjusts low/high velocity. A small amber marker identifies the root without
covering the keyboard's mapping colors. Roots are read-only here.
The full view labels fitting regions vertically and highlights selected effective
coverage. Velocity raster lines and labels occur every 5 steps, with heavier lines
at 0, 25, 50, 75, 100 and 125. The plot keeps a minimum usable height and scrolls
within short windows, leaving the action/status bars visible.
Four dashed guides mark the outer edges of selected
effective coverage, including grouped assignments. Individual member outlines
remain visible; the group envelope does not fill unmapped gaps. The blue editable
Program limits and handles remain separate and preview dragging locally.
Thin or overlapping labels yield to the selected
region, with complete names retained in tooltips.
Program list subtitles summarize assignments,
assigned banks, their distinct member Samples, and directly assigned Samples;
unresolved assignments are identified separately from confirmed targets.

Press and hold a keyboard key to audition every assignment whose effective
key/velocity range matches it, regardless of selection. The audition velocity
defaults to 100 and can be adjusted from 1 to 127. Releasing the key stops the
group, including held loops; focus loss or window closure also releases it.
Keyboard audition changes neither selection nor roots. The main window owns
playback and uses the current canonical Sample/Bank drafts. Unresolved or
unsupported members fail the group rather than playing a silently incomplete
subset. Source preparation is bounded to four concurrent requests and a shared
128 MiB working limit, with identical Wave Data reused across distinct voices.

This is a draft preview, not a complete sampler sound engine. It applies trim,
supported loops/reverse playback, fixed pitch, tuning, level and pan; active Bank
overrides and Program Easy Edit key-shift/tuning/level/pan offsets are included,
as is Program level. It does not emulate envelopes, filters, LFOs, effects,
Program transpose/portamento, output routing or MIDI/controller behavior.
Displayed Program coverage is the Easy Edit/source intersection before Program
transpose. Use the sampler to judge those additional sound-engine settings.

Effects routing uses the sampler's five fixed connection
patterns, not an unrestricted patch graph. Selecting a block opens its type,
levels, pan, width and destination beside the graph. The same independently
scrollable pane includes algorithm parameters below a divider. Ef1-3 and Ef4-6
routing selectors share a row when space permits; there is no separate Effects
subtab row. Amp EG uses the same graph-left, controls-right layout, stacking
the panes when the lower zone is narrow.
Numeric effect words use their exact stored ranges; these are not universally
MIDI-sized values or calibrated physical units.

The blue `+` identifies a later-generation effect or parameter domain, not every
parameter in a current Program. Raw effect IDs 0..54 (Through and the original
54 effects) share their 593 visible parameter domains across generations.
The first three effects retain the same sixteen-word layout; the type selector
is stored in a different header byte and is copied during legacy import.
Unused reset words can differ and are not treated as additional editable
parameters. Ef4-6 are marked A5000-only at the effect heading; their ordinary
level, pan and width controls do not each receive a redundant marker.

The storage revision controls which parameter model is shown. A current
Program uses the A4000/A5000 superset, with A5000-only selections identified;
this does not infer which hardware originally saved it. Native Programs omit
unavailable current-only pages. Multi mode retains the selected Program's
settings and identifies pages whose playback depends on the master Program.
Blue plus markers identify extended parameters and choices; their tooltips
distinguish A4000/A5000-format extensions from A5000-only features.
