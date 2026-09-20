# Sample Parameter Authoring

The `parameters` JSON object describes sampler-visible Sample (`SBNK`)
settings. The same fields are accepted for fresh Samples, existing-Sample
updates, existing-bank/member updates, and a fresh Sample Bank's
`parameter_overrides` object.

All fields are optional. An empty fresh Sample `parameters` object selects the
defaults below. An existing-Sample update or Sample Bank override must contain
at least one field. Omitted fields in an existing object are preserved. Omitted
fields in a fresh Sample receive the defaults below.

Existing Samples retain their stored parameter format on every ordinary edit.
`a3000_188` identifies a 188-byte A3000 parameter block, and
`a4000_a5000_224` identifies a 224-byte A4000/A5000 block: the shared 188-byte
prefix followed by a 36-byte extension. Header lengths, not physical allocation
or file padding, identify the format. Unsupported headers remain unknown.

The desktop badges `a3k` and `a4k/a5k` describe this stored format, not the
originating sampler, parameter compatibility, or hardware certification. Invalid
or later-only values in a native block produce separate warnings; they never
change its format badge. Output destinations requiring A5000 effects are a
separate model requirement within the later format.

The tables below describe fresh A4000/A5000 authoring. Existing A3000 Samples
have these differences: coarse tune is `-127..127`, pitch bend type is `0..13`,
AEG attack mode is `0..1`, controller device is `0..125`, controller function is
`0..21`, output 1 destination is `0..4`, and output 2 destination is `0..5`.
Native `velocity_crossfade` is a Boolean, native `portamento_type` is `0..1`,
and Sample EQ is implicitly Peak/Dip. Independent velocity crossfade widths,
sample portamento rate/time and shelf EQ require explicit later-format conversion.
Untouched unsupported values are preserved; edits must use the stored format's
supported range.

`convert_sbnk_format` is a separate, payload-digest-guarded transaction. The
desktop requires saving or discarding the draft first. Conversion preserves
Sample identity, name, relationships, Wave Data, unrelated bytes and trailing
padding. Upconversion initializes the extension from native controllers, outputs,
switches and fixed portamento defaults. The result always becomes `a4k/a5k`.
Downconversion is blocked unless all authoritative settings can be represented
without loss: native ranges, Peak/Dip EQ, crossfade widths 0/0 or 5/5, portamento
type 0/1 and rate/time 90, and no unknown nonzero extension data. Resolve blockers
manually and save before trying again. No automatic lossy mapping is performed.
Test authored media on the intended hardware; neither conversion certifies it.

## General, MIDI, Pitch, And Loop

| JSON field | Accepted value | Fresh default |
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

## Playback Window

`playback_window` is separate from the shared parameter block because
its bounds depend on backing PCM. Its JSON shape is
`"playback_window": {"start_frame": 65536, "length_frames": 1000}` alongside
`parameters`, not inside it. The positive-length absolute frame window must fit
every source and end at or before frame `16777216`. Fresh audio-backed Samples
default to the imported PCM span, excluding generated guard frames. Insertion
into an existing image defaults to the current Wave Data playback window.

An explicit window updates both stored start/length lanes and derived endpoints,
without trimming or changing PCM. Explicit loop coordinates remain absolute and
must lie inside the selected playback window. Nonrepeating modes default an
omitted loop, or an explicit zero start and zero length, to that window;
repeating modes still require an explicit non-empty loop.
This is not a bank-wide override: different members can have different sources
and playback extents. Existing Sample retargeting preserves its existing window.

## Filter, Expansion, Level, And EQ

| JSON field | Accepted value | Fresh default |
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
| `pan` | `-63..63`, or `-64` for random pan (`Rnd`) | `0` (center) |
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
Supplying an EQ value explicitly regenerates the vector even when that value
equals the value already stored. The coefficient vector is playback state,
not disposable padding: a discrepancy with the semantic values must not be
silently normalized during an unrelated edit.
Width affects Peak/Dip only; the two shelf types use their fixed stored-response
shape. HiShelv also limits the effective coefficient gain at low frequency
selections while retaining the requested semantic gain value.

For fresh authoring, nonzero `expand_detune` or `expand_dephase` selects the
supported one-source expanded-mono profile. Sparse updates also support these
scalars on an existing true stereo pair with distinct sources and no expanded
flag: the member bindings, channel windows and pitch values remain unchanged.
Retained expanded or duplicate-source pairs are not authorized for topology
changes by these scalar edits. Context-free registered templates retain their
separate restrictions.
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

| JSON field | Accepted value | Fresh default |
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

Output destinations use the lane-specific
[stored destination mappings](sampler-data.md#current-sample-output-destinations).
In particular, Ef1 is Output 1 value `2` but Output 2 value `7`; values `10..12`
refer to A5000 effects.

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
| Per-member sample rate | Derived from each referenced Wave Data object. |
| Full per-member wave-start addresses and playback lengths | Derived from `playback_window` and its source-dependent defaults when creating or inserting a Sample; nonzero starts are supported. An existing Sample update can explicitly supply `playback_window`, validated jointly with its resulting loops. Otherwise parameter updates and retargeting preserve the stored window. |
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
