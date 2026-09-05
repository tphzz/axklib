# Sample Parameter Authoring

`SampleParameters` is the one public model for sampler-visible Sample (`SBNK`)
parameters. The same model is used by:

- `SampleSpec.parameters` when creating a Sample;
- `UpdateSampleParametersOperation.parameters` when altering an existing Sample;
- `SampleBankSpec.parameter_overrides` for the current Sample Bank (`SBAC`)
  parameter state and immediate member-wide application; and
- the corresponding `parameters` and `parameter_overrides` JSON objects.

All fields are optional. An empty `SampleSpec.parameters` object selects the
fresh Sample defaults below. An existing-Sample update or Sample Bank override
must contain at least one field. Omitted fields in an existing object are
preserved. Omitted fields in a fresh Sample receive the defaults below.

## General, MIDI, Pitch, And Loop

| JSON / C++ field | Accepted value | Fresh default |
| --- | --- | --- |
| `fixed_pitch`, `key_crossfade`, `mono_mode` | Boolean | `false` |
| `sample_eq_type` | `0..2` | `0` |
| `midi_receive_channel` | `0..16` | `0` |
| `pitch_bend_type` | `0..12` | `0` |
| `pitch_bend_range` | `0..24` | `2` |
| `coarse_tune` | `-64..63` | `0` |
| `root_key` | MIDI note `0..127` | `60` |
| `fine_tune_cents` | `-63..63` cents | `0` |
| `key_low` | `0..127`, or `255` for `=Orig` | `0` |
| `key_high` | `0..127`, or `128` for `=Orig` | `127` |
| `loop_mode` | `0..5`: `-->`, `->0`, `->0->`, `<--`, `One->`, `One<-` | `4` (`One->`) |
| `loop_tempo_hundredths` | `8000..15999` | `9000` (90.00 BPM) |
| `loop_start_frame`, `loop_length_frames` | A contained playback window; repeating modes require a non-empty window | Full Wave Data span |
| `wave_start_velocity_sensitivity` | `-63..63` | `0` |

The effective key high value must not precede the effective key low value.
`=Orig` resolves to `root_key` for this validation. Root key, fine tune, loop
start, and loop length are shared Sample settings: the writer mirrors them to
every active stereo member and recomputes the associated internal caches.
Numbered MIDI receive channels use zero-based storage: raw `0..15` displays as
`01..16`; raw `16` is `Bch`.

## Filter, Expansion, Level, And EQ

| JSON / C++ field | Accepted value | Fresh default |
| --- | --- | --- |
| `filter_type` | `0..16` | `0` (bypass) |
| `filter_cutoff` | `0..127` | `127` |
| `filter_q_width` | `0..31` | `4` |
| `filter_scaling_break1`, `filter_scaling_break2` | MIDI notes `0..127`; break 1 must not exceed break 2 | `0`, `127` |
| `filter_scaling_cutoff1`, `filter_scaling_cutoff2` | `-127..127` | `0`, `0` |
| `filter_velocity_to_cutoff`, `filter_velocity_to_q_width` | `-63..63`, plus `64..68` for the five random modes | `0`, `0` |
| `filter_gain` | `-31..31` | `0` |
| `expand_detune` | `-7..7` | `0` |
| `expand_dephase`, `expand_width` | `-63..63` | `0`, `63` |
| `random_pitch` | `0..63` | `0` |
| `level` | `0..127` | `100` |
| `pan` | `-64..63` | `0` (center) |
| `velocity_low_limit` | `0..127` | `0` |
| `velocity_offset` | `-127..127` | `0` |
| `velocity_low`, `velocity_high` | `0..127`; low must not exceed high | `0`, `127` |
| `velocity_sensitivity` | `-127..127` | `0` |
| `level_scaling_break1`, `level_scaling_break2` | MIDI notes `0..127`; break 1 must not exceed break 2 | `0`, `127` |
| `level_scaling_level1`, `level_scaling_level2` | `0..127` | `127`, `127` |
| `alternate_group` | `0..16` | `0` (off) |
| `sample_eq_frequency` | `4..58` | `26` |
| `sample_eq_gain_db` | `-12..12` dB | `0` dB |
| `sample_eq_width_tenths` | `10..120` | `10` |
| `filter_cutoff_distance` | `-63..63` | `0` |

The four Sample EQ fields also maintain the five signed Q13 biquad coefficients
stored in the Sample or Sample Bank parameter block. Supplying any EQ field
recomputes the complete coefficient vector from the resulting semantic values;
updates that do not touch EQ preserve an existing vector byte-for-byte.
Width affects Peak/Dip only; the two shelf types use their fixed stored-response
shape. HiShelv also limits the effective coefficient gain at low frequency
selections while retaining the requested semantic gain value.

Nonzero `expand_detune` or `expand_dephase` selects the supported expanded-mono
profile. It is valid only for a Sample with one Wave Data source. Duplicate-
source expanded mono remains preservation-only and cannot be authored.
Sample EQ frequency is a stored selection, not a frequency in hertz. For
example, raw `30` displays as `630Hz`.

## Envelopes

Envelope values are nested under `feg`, `peg`, and `aeg`. A present envelope
object must contain at least one field and accepts only the fields in its own
row set.

| Object | Fields | Accepted value | Fresh default |
| --- | --- | --- | --- |
| `feg` | `attack_rate`, `decay_rate`, `release_rate` | `0..127` | `127` |
| `feg` | `init_level`, `attack_level`, `sustain_level`, `release_level` | `-127..127` | `0` |
| `feg` | `rate_key_scaling` | `-7..7` | `0` |
| `feg` | `rate_velocity_sensitivity`, `attack_level_velocity_sensitivity`, `level_velocity_sensitivity` | `-63..63` | `0` |
| `peg` | `attack_rate`, `decay_rate`, `release_rate` | `0..127` | `127` |
| `peg` | `init_level`, `attack_level`, `sustain_level`, `release_level` | `-127..127` | `0` |
| `peg` | `rate_key_scaling` | `-7..7` | `0` |
| `peg` | `rate_velocity_sensitivity`, `level_velocity_sensitivity` | `-63..63` | `0` |
| `peg` | `range` | `-63..63` | `12` |
| `aeg` | `attack_rate`, `decay_rate` | `0..127` | `127` |
| `aeg` | `release_rate` | `0..127` | `126` |
| `aeg` | `sustain_level` | `0..127` | `127` |
| `aeg` | `attack_mode` | `0..2` | `0` |
| `aeg` | `rate_key_scaling` | `-7..7` | `0` |
| `aeg` | `rate_velocity_sensitivity` | `-63..63` | `0` |

## LFO, Controllers, Outputs, And Portamento

| JSON / C++ field | Accepted value | Fresh default |
| --- | --- | --- |
| `lfo.wave` | `0..3` | `1` |
| `lfo.speed` | Display value `1..128`; storage uses value minus one | `40` |
| `lfo.delay_time` | `0..127` | `0` |
| `lfo.key_on_sync` | Boolean | `true` |
| `lfo.cutoff_mod_phase_invert`, `lfo.pitch_mod_phase_invert` | Boolean | `false` |
| `lfo.cutoff_mod_depth`, `lfo.pitch_mod_depth`, `lfo.amp_mod_depth` | `0..127` | `0` |
| `velocity_xfade_high`, `velocity_xfade_low` | `0..127` | `0` |
| `output1_destination`, `output2_destination` | `0..12` | `1`, `0` |
| `output1_level`, `output2_level` | `0..127` | `127`, `127` |
| `portamento_type` | `0..5` | `0` (off) |
| `portamento_rate`, `portamento_time` | `1..127` | `90`, `90` |

Controllers are represented by a `controls` object whose keys are the strings
`"1"` through `"6"`. Each present controller object must contain at least one
of these fields:

| Controller field | Accepted value |
| --- | --- |
| `device` | `0..126` |
| `function` | `0..36` |
| `type` | `0..3` |
| `range` | `-63..63` |

Fresh controller defaults are `(device, function, type, range)` values
`(74,4,1,32)`, `(71,5,1,32)`, `(73,11,1,-32)`, `(72,12,1,-32)`, and
`(0,0,0,0)` for controllers 5 and 6. The duplicated physical controller records
are an internal storage detail; one public controller value updates both copies.
The numeric values follow the sampler's stored enums. For example, Function `4`
is `Cutoff Bias`, Function `5` is `Filter Q/Width`, and Type `1` is
`-/+offset`.

## Fresh Sample Bank State

A fresh Sample Bank stores its own canonical current-parameter state. Its
writable defaults match the tables above except that its unspecialized
`loop_mode` state is `0` (`-->`). Geometry and topology fields that are derived
for a Sample are zero or canonical placeholders in the bank state. Two internal
AEG-group transport bytes also use the established bank profile and are not
public input.

`parameter_overrides` replaces only the supplied fields in that state and
applies exactly those fields to every member Sample. Unspecified fields are not
propagated, so each member retains its own values. Application is atomic and the
Sample Bank's pending-propagation bits remain clear.
Fresh image creation also derives the Sample Bank's linked-Program bitmap from
Program assignments. Program insertion and deletion update the same bitmap
transactionally; callers cannot provide it as raw parameter state.

## Derived And Read-Only State

The following object state is deliberately not public authoring input:

| State | Policy |
| --- | --- |
| Sample Bank membership, mono/stereo, and expanded topology flags | Derived from graph membership and active Wave Data topology. |
| Per-member sample rate and Wave Data length | Derived from each referenced Wave Data object. |
| Full per-member wave-start addresses | Read-only for existing objects; fresh supported profiles use zero. |
| Pitch, loop-end, Program-portamento, and other playback caches | Recomputed when their public source values change. |
| Linked Program bitmaps and Sample Bank pending-propagation state | Derived from relationships; pending bits are clear after immediate application. |
| Reserved bytes and opaque packed-bit lanes | Canonical defaults in fresh objects and byte-preserved in existing objects. |

JSON rejects these as unknown fields rather than accepting raw offsets, caches,
flags, aliases, or obsolete flat Sample fields.

## JSON Example

```json
{
  "name": "Mapped Sample",
  "waveform_id": "wave",
  "parameters": {
    "root_key": 64,
    "key_low": 24,
    "key_high": 96,
    "filter_cutoff": 91,
    "feg": {"attack_rate": 81},
    "lfo": {"speed": 88, "key_on_sync": false},
    "controls": {"1": {"device": 65, "function": 36, "type": 3, "range": -63}},
    "output1_destination": 12,
    "portamento_type": 1
  }
}
```
