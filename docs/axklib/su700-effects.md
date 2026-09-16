# SU700 Effects Records

This reference describes the `Efsn` portion of [SU700 song files](su700.md#ssq-song-storage).
Track setup, ordinary sample parameters and sequence events are described in
[Song And Track Records](su700-song.md). Offsets, type IDs and parameter slots
below are decimal; multi-byte serialized values are big-endian.

## Scene Framing

`Efsn` has a four-byte tag, a u32be body size, then eight effect-scene records.
Each record starts with a one-byte state. State one carries a 770-byte payload.
Scene zero also carries that payload when the optional SSQ extension selector
is below 128, irrespective of its state. The selector is the final extension
byte, or 255 when the extension is absent. State zero otherwise denotes absence;
other states have no general safe interpretation.

The native writer always emits scene-zero payload, but that writer convention
must not replace the conditional reader rule for older or unextended files.
Eight populated records occupy 6168 body bytes; one populated and seven absent
records occupy 778 bytes. Payload offsets exclude the leading state byte:

| Offset | Shape | Contents |
| --- | --- | --- |
| 0..730 | 43 records of 17 bytes | Track/control records indexed by identity 0..42. |
| 731..769 | 3 records of 13 bytes | Effect units 0..2 (displayed as EFFECT 1..3). |

Capture and ordinary recall operate on control identities 1..42 and all three
units. Identity zero is serialized but skipped by those operations; it is not
an additional visible sample track or established padding. Preserve it.

## Track And Control Records

Record identity `i` starts at payload offset `17*i`. Identity 1 is MASTER,
2 AUDIO IN, and 3..42 the sample identities listed in
[Track Identities](su700.md#track-identities).

| Offset | Type/size | Meaning and applicability |
| --- | --- | --- |
| 0 | byte | Current assigned effect index in insertion routing; 3 detached/default. Not an independent free-form parameter. |
| 1..3 | 3 bytes | EFFECT 1..3 knob values; modulation depends on the corresponding effect type. |
| 4 | byte | Output selector copy, normal 0..9. |
| 5 | byte | PAN, exposed here for MASTER/AUDIO IN. |
| 6 | byte | LEVEL, exposed here for MASTER/AUDIO IN. |
| 7 | byte | EQ HI GAIN, MASTER control. |
| 8 | byte | EQ LO GAIN, MASTER control. |
| 9 | byte | EQ HI FRQ, MASTER control. |
| 10 | byte | EQ LO FRQ, MASTER control. |
| 11 | byte | MAIN knob function code for MASTER/AUDIO IN. |
| 12 | byte | MAIN pad function code for MASTER/AUDIO IN. |
| 13..15 | 3 bytes | Per-effect assignment flags, normal 0/1, for units 0..2. |
| 16 | byte | Unspecified meaning; copied with the record, not cleared by the ordinary record reset. |

MAIN function codes share the [track code mapping](su700-song.md#main-control-codes),
but the knob/pad byte order is reversed relative to Trkp bytes 8/9. The repeated
record shape does not establish that every control is independently applicable
to every identity. Ordinary sample pan/level/EQ are separately stored in sample
scenes; no general synchronization rule with these repeated bytes is specified.

### Routing And Applicability

Output selector zero is STEREO OUT, 1..6 are AS 1..6, and 7..9 are AS 1+2,
AS 3+4 and AS 5+6. Loading without the alternate-output capability clears byte
4 for identities 1..42, including state retained for absent scenes. The Trkp
routing word and this byte copy must remain distinct: their initialization
relationship is not fully specified and cannot support automatic repair.

Assignment flags select track/effect relationships, rather than globally
bypassing a unit. The assignment setter accepts flags 0/1 for identities 2..42;
MASTER and identity zero are excluded by that setter. Insertion-effect editing
checks for another assigned effect and can report a conflict instead of toggling.
Full conflict resolution is not specified here.

For insertion routing, detaching a matching assignment restores control byte 0
to 3 and the unit target to 43. System effects use a different routing path.
With STEREO OUT selected, one output-restoration path scans assignment flags
in unit order and applies the first flag equal to one. This is not a validity
rule permitting arbitrary multiple insertion assignments.

## Effect Units

Unit `u` starts at payload offset `731 + 13*u`:

| Offset | Type/size | Meaning and applicability |
| --- | --- | --- |
| 0 | byte | Assignment/target state; 43 is an unassigned sentinel in insertion routing. Complete interpretation across types remains unspecified. |
| 1 | byte | Effect type ID, 0..42. |
| 2 | byte | Shared unit tempo selector, normal 0..18, where supported. |
| 3..7 | 5 bytes | Type-specific parameter slots 1..5. |
| 8 | byte | LEVEL. |
| 9 | byte | PAN. |
| 10 | byte | Unspecified meaning; reset clears it, which does not establish padding. |
| 11 | byte | EF2 SEND, applicable to unit 0 only. |
| 12 | byte | EF3 SEND, applicable to units 0/1 only. |

Type IDs are specific to SU700, not A-series Program effect IDs. Selecting a
type resets all five type-specific parameter bytes from its defaults. Editing
those parameters applies the type-specific raw clamps in the tables below;
bulk scene loading does not imply the same validation. Retain hidden slots,
inapplicable sends and unknown bytes on unrelated edits.

### Scene Application

Snapshot copying and live application are distinct. Ordinary copying requires
state one and preserves all bytes of the copied records. Application normally
requires state one too, but scene zero has an initialization exception that can
apply existing live state even when no snapshot was copied. An absent scene
therefore cannot be replaced by zero-filled records.

Live application handles the three units first, then assignment flags and
controls for identities 1..42. Type changes precede their parameter application.
This outer order does not establish exact audible transition timing or complete
DSP transformations. The [sample-scene mute order](su700-song.md#scene-application-and-mute)
is a separate part of the same scene recall.

## Tempo Selector

The selector is shared by the whole effect unit, not separately saved for every
track's effect knob. Zero selects free mode. Selectors 1..18 use these nominal
durations; the integer factor is part of the interval calculation:

| Selector | Factor | Nominal duration |
| --- | --- | --- |
| 0 | 0 | Free mode; no synchronized interval. |
| 1 | 60 | Thirty-second note |
| 2 | 120 | Sixteenth note |
| 3 | 240 | Eighth note |
| 4 | 480 | Quarter note |
| 5 | 960 | Half note |
| 6 | 1920 | Whole note |
| 7 | 40 | Thirty-second-note triplet |
| 8 | 80 | Sixteenth-note triplet |
| 9 | 160 | Eighth-note triplet |
| 10 | 320 | Quarter-note triplet |
| 11 | 640 | Half-note triplet |
| 12 | 1280 | Whole-note triplet |
| 13 | 90 | Dotted thirty-second note |
| 14 | 180 | Dotted sixteenth note |
| 15 | 360 | Dotted eighth note |
| 16 | 720 | Dotted quarter note |
| 17 | 1440 | Dotted half note |
| 18 | 2880 | Dotted whole note |

For a supported nonzero selector and valid nonzero tempo,
`interval_ms = factor * 1250 / raw_tempo`, with tempo in tenths of BPM.
Selector four at raw tempo 1200 gives 500 ms. This is the base interval;
effect-specific scaling and clamps mean it is not necessarily an audible repeat
period. Selector 19 and tempo zero have no safe specified downstream behavior,
even where a byte setter accepts them.

The per-type catalogue identifies all 18 sync-capable types. Similar names do
not imply equal capability: 3DELAY is free-only while 2DELAY and 1DELAY support
sync. Tempo changes update derived timing without rewriting the stored selector.
Free mode restores type-specific working defaults rather than a historical
pre-sync value. Working timing/coefficient values are not additional SSQ fields.

Complete DSP application remains unspecified, including two sync/free
asymmetries for 2DELAY and DLY+PAN and some effect-knob interactions. Do not
change saved parameters to compensate for presumed audible defects or promise
bit-exact DSP behavior from the selector tables.

## Parameter Display Encodings

The catalogue's display column uses the following descriptive names, not stored
formatter IDs. Apply mappings only within each parameter's own raw editing
range. A lookup table can contain more entries than that range permits.

| Display | Mapping |
| --- | --- |
| Unsigned | Raw value as a decimal index; no implied Hz, milliseconds or dB. |
| Signed-64 | `raw - 64`. |
| Threshold | `raw - 127`; normal 79..121 displays -48..-6. |
| Phase | `3 * (raw - 64)`; normal 4..124 displays -180..180. |
| Amplifier | 0 OFF, 1 STACK, 2 COMBO, 3 TUBE. |
| Release | Indices 0..15: 10, 15, 25, 35, 45, 55, 65, 75, 85, 100, 115, 140, 170, 230, 340, 680. |
| Ratio | Indices 0..7: 1.0, 1.5, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0. |
| Mono/stereo | 0 MONO, 1 STRO. |
| Pan pattern | 0 `L<>R`, 1 `L>R`, 2 `L<R`, 3 `L@`, 4 `R@`, 5 `L/R`; exact physical display glyphs unspecified. |
| Polarity | 0 NORML, 1 INVRS. |
| Channel | 0 L, 1 R, 2 L/R. |
| Letter | Zero-based A..H table; use only the parameter's actual range. |
| Resolution | Indices 0..8: 1, 1/2, 1/4, 1/8, 1/16, 1/32, 1/64, 1/128, 1/256. |
| Wet mode | 0 OFF, 1 WET, 2 +DRY. |
| Attack | Indices 0..19: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 40. |
| Dry/wet | Raw 1..127 centered at 64; lower favors dry, higher favors wet, magnitude `abs(raw - 64)`. |
| Early/reverb | Raw 1..127 centered at 64; lower favors early reflections, higher favors reverb, magnitude `abs(raw - 64)`. |
| Not exposed | Stored slot without a visible control or established display meaning. |

The indexed attack/release numbers do not by themselves establish physical
units. Likewise the phase numeric scale does not specify the complete DSP
transfer function. Letter-valued modulation controls must not be relabelled
as sine/triangle/saw without an established mapping.

Dry/wet text ranges from `D63>W` at raw 1 through `D01>W` at 63, centered
`D_W` at 64, then `D<W01` at 65 and `D<W63` at 127. Early/reverb uses
`E63>R`, `E01>R`, `E_R`, `E<R01`, `E<R63` correspondingly. These describe
display text, not an equal-power mixing law or guaranteed physical LCD glyphs.

### Domain Qualifications

Four controls have differing printed descriptions and device-editing domains:

- LO RESO MODOFST uses raw 1..127, rather than including zero.
- LO RESO RESOLTN uses indices 0..6, displaying 1 through 1/64; larger printed
  denominators do not extend this control's range.
- ATKLOFI RESOLTN index zero displays 1, not zero; indices 0..4 end at 1/16.
- NOISAMB DLY LVL uses unsigned 0..127, rather than an A..D enumeration.

These distinctions do not establish a historical revision mapping. Preserve
off-domain stored bytes rather than silently converting them to the printed
alternative. Editing clamps are not universal file-validity requirements.

## Type And Parameter Catalogue

Type IDs are zero-based; parameter slot numbers below are one-based. The byte
offset is within the 13-byte effect-unit record. All five slots are stored for
each type, including the five slots without visible controls. A hidden slot's
editing clamp does not make an arbitrary loaded value disposable padding.

### 0: TECHMOD

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MOD SPD | 0..127 | Unsigned |
| 2 | 4 | MODDPTH | 0..127 | Unsigned |
| 3 | 5 | MOD HPF | 0..52 | Unsigned |
| 4 | 6 | MODGAIN | 52..76 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 1: AUTOSYN

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MOD SPD | 0..127 | Unsigned |
| 2 | 4 | MODWAVE | 0..3 | Letter |
| 3 | 5 | MODDPTH | 0..127 | Unsigned |
| 4 | 6 | MODOFST | 1..127 | Signed-64 |
| 5 | 7 | DLY LVL | 0..127 | Unsigned |

### 2: SCRATCH

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | INPUT | 0..127 | Unsigned |
| 2 | 4 | DELAY | 0..127 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | PANDPTH | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 3: JUMP

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | TYPE | 0..2 | Letter |
| 3 | 5 | JMPWAVE | 0..3 | Letter |
| 4 | 6 | RESOLTN | 0..8 | Resolution |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 4: PITCH1

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | PITCH | 40..88 | Signed-64 |
| 2 | 4 | FINE | 14..114 | Signed-64 |
| 3 | 5 | INITDLY | 0..127 | Unsigned |
| 4 | 6 | FBLEVEL | 1..127 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 5: PITCH2

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FINE1 | 14..114 | Signed-64 |
| 2 | 4 | FINE2 | 14..114 | Signed-64 |
| 3 | 5 | INITDLY | 0..127 | Unsigned |
| 4 | 6 | FBLEVEL | 1..127 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 6: VCECNCL

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | LOW ADJ | 0..26 | Unsigned |
| 2 | 4 | HI ADJ | 0..26 | Unsigned |
| 3 | 5 | Not exposed | 0..0 | Not exposed |
| 4 | 6 | Not exposed | 0..0 | Not exposed |
| 5 | 7 | Not exposed | 0..0 | Not exposed |

### 7: AMBIENC

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DLYTIME | 0..127 | Unsigned |
| 2 | 4 | OUT_PHS | 0..1 | Polarity |
| 3 | 5 | LOWGAIN | 52..76 | Signed-64 |
| 4 | 6 | HI GAIN | 52..76 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 8: LO RESO

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MODDPTH | 0..127 | Unsigned |
| 2 | 4 | MODOFST | 1..127 | Unsigned |
| 3 | 5 | RESOLTN | 0..6 | Resolution |
| 4 | 6 | PHASINV | 0..2 | Wet mode |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 9: NOISY

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DRIVE | 0..127 | Unsigned |
| 2 | 4 | MODDPTH | 0..10 | Unsigned |
| 3 | 5 | LPF FRQ | 34..60 | Unsigned |
| 4 | 6 | LPF Q | 10..120 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 10: ATKLOFI

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | SENSITV | 0..127 | Unsigned |
| 2 | 4 | RESOLTN | 0..4 | Resolution |
| 3 | 5 | PEAKFRQ | 14..54 | Unsigned |
| 4 | 6 | LPF FRQ | 34..60 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 11: RADIO

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MOD LPF | 0..52 | Unsigned |
| 2 | 4 | MODLPFQ | 10..120 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | LPF FRQ | 34..60 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 12: TURNTBL

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | NOISLVL | 0..127 | Unsigned |
| 2 | 4 | NS TONE | 0..6 | Unsigned |
| 3 | 5 | NSLPF Q | 10..120 | Unsigned |
| 4 | 6 | CLICK | 0..127 | Unsigned |
| 5 | 7 | DRYNOIS | 0..127 | Unsigned |

### 13: DIST

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DRIVE | 0..127 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | MIDFREQ | 14..54 | Unsigned |
| 4 | 6 | OUT LVL | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 14: OVERDRV

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DRIVE | 0..127 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | MIDFREQ | 14..54 | Unsigned |
| 4 | 6 | OUT LVL | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 15: AMPSIM

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DRIVE | 0..127 | Unsigned |
| 2 | 4 | AMPTYPE | 0..3 | Amplifier |
| 3 | 5 | LPF FRQ | 34..60 | Unsigned |
| 4 | 6 | EDGE | 0..127 | Unsigned |
| 5 | 7 | OUT LVL | 0..127 | Unsigned |

### 16: COMP

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | THRSHLD | 79..121 | Threshold |
| 2 | 4 | ATTACK | 0..19 | Attack |
| 3 | 5 | RELEASE | 0..15 | Release |
| 4 | 6 | RATIO | 0..7 | Ratio |
| 5 | 7 | OUT LVL | 0..127 | Unsigned |

### 17: COMP+DS

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | THRSHLD | 79..121 | Threshold |
| 2 | 4 | RATIO | 0..7 | Ratio |
| 3 | 5 | DRIVE | 0..127 | Unsigned |
| 4 | 6 | LPF FRQ | 34..60 | Unsigned |
| 5 | 7 | OUT LVL | 0..127 | Unsigned |

### 18: TWAH+DS

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FRQOFST | 0..127 | Unsigned |
| 2 | 4 | RESO | 10..120 | Unsigned |
| 3 | 5 | DRIVE | 0..127 | Unsigned |
| 4 | 6 | DR LPF | 34..60 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 19: TWAH+OD

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FRQOFST | 0..127 | Unsigned |
| 2 | 4 | RESO | 10..120 | Unsigned |
| 3 | 5 | DRIVE | 0..127 | Unsigned |
| 4 | 6 | DR LPF | 34..60 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 20: AWAH+DS

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | FRQOFST | 0..127 | Unsigned |
| 3 | 5 | RESO | 10..120 | Unsigned |
| 4 | 6 | DRIVE | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 21: AWAH+OD

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | FRQOFST | 0..127 | Unsigned |
| 3 | 5 | RESO | 10..120 | Unsigned |
| 4 | 6 | DRIVE | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 22: AUTOPAN

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | L/RDPTH | 0..127 | Unsigned |
| 2 | 4 | F/RDPTH | 0..127 | Unsigned |
| 3 | 5 | DIRECTN | 0..5 | Pan pattern |
| 4 | 6 | LOWGAIN | 52..76 | Signed-64 |
| 5 | 7 | HI GAIN | 52..76 | Signed-64 |

### 23: TREMOLO

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | LFOFREQ | 0..127 | Unsigned |
| 2 | 4 | AMDEPTH | 0..127 | Unsigned |
| 3 | 5 | LOWGAIN | 52..76 | Signed-64 |
| 4 | 6 | HI GAIN | 52..76 | Signed-64 |
| 5 | 7 | INMODE | 0..1 | Mono/stereo |

### 24: TRM_BPM

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | AMDEPTH | 0..127 | Unsigned |
| 2 | 4 | PMDEPTH | 0..127 | Unsigned |
| 3 | 5 | PHASE | 4..124 | Phase |
| 4 | 6 | INMODE | 0..1 | Mono/stereo |
| 5 | 7 | LOWGAIN | 52..76 | Signed-64 |

### 25: ROTARY

Insertion effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | LFOFREQ | 0..127 | Unsigned |
| 2 | 4 | DEPTH | 0..127 | Unsigned |
| 3 | 5 | LOWGAIN | 52..76 | Signed-64 |
| 4 | 6 | HI GAIN | 52..76 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 26: CHORUS

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | LOWGAIN | 52..76 | Signed-64 |
| 3 | 5 | HI GAIN | 52..76 | Signed-64 |
| 4 | 6 | DRY/WET | 1..127 | Dry/wet |
| 5 | 7 | INMODE | 0..1 | Mono/stereo |

### 27: PHASER

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | PHSHIFT | 0..127 | Unsigned |
| 3 | 5 | FBLEVEL | 1..127 | Signed-64 |
| 4 | 6 | STAGE | 4..12 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 28: FLANGER

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | DEPTH | 0..127 | Unsigned |
| 2 | 4 | FBLEVEL | 1..127 | Signed-64 |
| 3 | 5 | OFFSET | 0..63 | Unsigned |
| 4 | 6 | PHASE | 4..124 | Phase |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 29: FLNGPAN

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FLN DLY | 0..127 | Unsigned |
| 2 | 4 | PAN DLY | 0..127 | Unsigned |
| 3 | 5 | PAN FB | 1..127 | Signed-64 |
| 4 | 6 | DLY LVL | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 30: NOISDLY

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MOD SPD | 0..127 | Unsigned |
| 2 | 4 | MODDEPTH | 0..127 | Unsigned |
| 3 | 5 | MODWAVE | 0..3 | Letter |
| 4 | 6 | FBLEVEL | 1..127 | Signed-64 |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 31: NOISAMB

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | MOD SPD | 1..127 | Unsigned |
| 2 | 4 | MODDPTH | 0..127 | Unsigned |
| 3 | 5 | DLY LVL | 0..127 | Unsigned |
| 4 | 6 | AMDEPTH | 0..127 | Unsigned |
| 5 | 7 | DRY/WET | 1..127 | Dry/wet |

### 32: FLOWPAN

Insertion effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | PAN SPD | 0..127 | Unsigned |
| 2 | 4 | DLY MIX | 0..127 | Unsigned |
| 3 | 5 | FBLEVEL | 1..127 | Signed-64 |
| 4 | 6 | FBHIDMP | 1..10 | Unsigned |
| 5 | 7 | PRPANDP | 0..127 | Unsigned |

### 33: 3DELAY

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | TIME L | 0..127 | Unsigned |
| 2 | 4 | TIME R | 0..127 | Unsigned |
| 3 | 5 | TIME C | 0..127 | Unsigned |
| 4 | 6 | FB TIME | 0..127 | Unsigned |
| 5 | 7 | FBLEVEL | 1..127 | Signed-64 |

### 34: 2DELAY

System effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FBLEVEL | 1..127 | Signed-64 |
| 2 | 4 | FBHIDMP | 1..10 | Unsigned |
| 3 | 5 | LOWGAIN | 52..76 | Signed-64 |
| 4 | 6 | HI GAIN | 52..76 | Signed-64 |
| 5 | 7 | Not exposed | 0..0 | Not exposed |

### 35: 1DELAY

System effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FBLVL | 1..127 | Signed-64 |
| 2 | 4 | FBHIDMP | 1..10 | Unsigned |
| 3 | 5 | LOWGAIN | 52..76 | Signed-64 |
| 4 | 6 | HI GAIN | 52..76 | Signed-64 |
| 5 | 7 | Not exposed | 0..0 | Not exposed |

### 36: X-DELAY

System effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FBLEVEL | 1..127 | Signed-64 |
| 2 | 4 | FBHIDMP | 1..10 | Unsigned |
| 3 | 5 | INSELECT | 0..2 | Channel |
| 4 | 6 | LOWGAIN | 52..76 | Signed-64 |
| 5 | 7 | HI GAIN | 52..76 | Signed-64 |

### 37: DLY+PAN

System effect. Tempo synchronization: supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | FBLEVEL | 1..127 | Signed-64 |
| 2 | 4 | FBHIDMP | 1..10 | Unsigned |
| 3 | 5 | PANDPTH | 0..127 | Unsigned |
| 4 | 6 | EQ GAIN | 52..76 | Signed-64 |
| 5 | 7 | EQ FREQ | 4..40 | Unsigned |

### 38: HALL

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | REVTIME | 0..69 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | ER/REV | 1..127 | Early/reverb |
| 5 | 7 | DIFFUSN | 0..10 | Unsigned |

### 39: ROOM

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | REVTIME | 0..69 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | ER/REV | 1..127 | Early/reverb |
| 5 | 7 | DIFFUSN | 0..10 | Unsigned |

### 40: STAGE

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | REVTIME | 0..69 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | ER/REV | 1..127 | Early/reverb |
| 5 | 7 | DIFFUSN | 0..10 | Unsigned |

### 41: PLATE

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | REVTIME | 0..69 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | ER/REV | 1..127 | Early/reverb |
| 5 | 7 | DIFFUSN | 0..10 | Unsigned |

### 42: CANYON

System effect. Tempo synchronization: not supported.

| Slot | Byte offset | Control | Raw editing range | Display |
| --- | --- | --- | --- | --- |
| 1 | 3 | REVTIME | 0..69 | Unsigned |
| 2 | 4 | LPF FRQ | 34..60 | Unsigned |
| 3 | 5 | HPF FRQ | 0..52 | Unsigned |
| 4 | 6 | ER/REV | 1..127 | Early/reverb |
| 5 | 7 | DIFFUSN | 0..10 | Unsigned |
