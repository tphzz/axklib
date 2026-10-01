---
title: SU700 Song And Track Records
---

# SU700 Song And Track Records

This reference describes the SSQ records framed by [SU700 File Layout And Sample Data](su700.md#ssq-song-storage).
Effects scenes are described separately in [SU700 Effects Records](su700-effects.md).
Offsets and counts below are decimal unless prefixed with `0x`; offsets in
tables are relative to the named body or snapshot. Multi-byte fields are
big-endian. `byte` denotes raw storage without an implied universal domain;
`i8` denotes a signed two's-complement byte.

Normal editing ranges describe device controls, not validation applied by every
file-load path. Preserve values outside those ranges and fields with unspecified
semantics on unrelated operations. These maps do not establish unrestricted
fresh authoring or complete playback emulation.

## Common Song Settings

The `Comn` body has 46 bytes:

| Offset | Type/size | Meaning and normal domain |
| --- | --- | --- |
| 0..1 | u16be | Tempo in tenths of BPM; 400..2999 means 40.0..299.9 BPM. |
| 2 | byte | Beats per measure; 1..4 for the supported 1/4..4/4 choices. |
| 3 | byte | Scaled beat duration; 24 for those quarter-note meters, not a literal denominator. |
| 4..5 | u16be | Unspecified meaning; preserve even when zero. |
| 6..21 | 8 u16be | Marker positions in sixteenth notes; `0xffff` means unset. |
| 22 | byte | MTC offset hours, 0..23. |
| 23 | byte | MTC offset minutes, 0..59. |
| 24 | byte | MTC offset seconds, 0..59. |
| 25 | byte | MTC offset frames; editing range 0..29. |
| 26..42 | 17 bytes | MIDI controller assignments in the order below. |
| 43 | byte | Saved clock source: 0 INTERNAL, 1 EXTERNAL, 2 MTC SLAVE. |
| 44 | byte | MASTER mute: 0 on, 1 muted. |
| 45 | byte | AUDIO IN mute: 0 on, 1 muted. |

### Clock, Meter And Position

The saved clock source is consumed but not directly restored by the common
record reader. Tempo saving selects the internal tempo for INTERNAL and the
alternate live tempo otherwise; loading places the saved word into internal
tempo storage without applying the editing clamp. Saved clock and tempo must
not be interpreted as a guarantee of the live clock state after loading.

Tempo event `0x7b` is clock-dependent: INTERNAL applies the 400..2999 clamp;
EXTERNAL skips the update; MTC SLAVE has a transport-dependent update path
without that same clamp. Noncanonical values are not safe just because the
common record can retain them.

Meter event `0x7e` stores the high value byte as beats per measure and six
times the low value byte as Comn byte 3, retaining the low byte of the product.
For example, event value `0x0404` corresponds to common bytes `04 18` (4/4).
The two representations are not interchangeable.

Marker zero is the song origin, not an empty marker. Marker recall uses coarse
positions and is bounded by the active transport limit. Storage quantizes
relative to a remembered meter boundary with stride
`floor(Comn[3] / 6) * Comn[2]` coarse units: 16 for 4/4. Do not require all
markers to be globally sorted or multiples of 16 across meter changes.
Marker button operations are distinct from recorded scene events.

The MTC tuple is an offset subtracted from incoming timecode. Its frame range
does not select a frame rate; incoming 24/25/30-rate handling is separate.
Complete drop-frame, rollover and negative-offset rules remain unspecified,
so a fixed conversion of these bytes to milliseconds is not defined.

### MIDI Controller Assignments

Each byte is a controller number; zero disables the assignment rather than
assigning controller zero. The ordinary MIDI number domain is 1..127 when
enabled. The file reader does not sanitize all larger bytes.

| Comn offset | Parameter |
| --- | --- |
| 26 | LEVEL |
| 27 | PAN |
| 28 | PITCH |
| 29 | ATTACK |
| 30 | RELEASE |
| 31 | SAMPLE LNGTH |
| 32 | LFO SPEED |
| 33 | LFO AMP DPTH |
| 34 | LFO FIL DPTH |
| 35 | LFO PIT DPTH |
| 36 | EQ HI GAIN |
| 37 | EQ LO GAIN |
| 38 | FILTR CUTOFF |
| 39 | RESONANCE |
| 40 | EFFECT 1 |
| 41 | EFFECT 2 |
| 42 | EFFECT 3 |

Normal editing rejects a nonzero controller already assigned elsewhere. During
receive-lookup initialization, later duplicate saved assignments instead replace
earlier mappings. This difference does not justify rejecting or rewriting a
loaded duplicate solely from the editor's uniqueness rule. SCRATCH is not an
eighteenth byte appended to this vector.

## Track Configuration

There are forty `Trkp` bodies of 64 bytes in the
[stored sample-slot order](su700.md#track-identities), independently of the
number of event tracks. Mute, level, pan and most performance parameters are
in sample scenes/events, not extra fields in this configuration record.

| Offset | Type/size | Meaning and normal encoding |
| --- | --- | --- |
| 0 | byte | Track loop length in quarter-note beats; editable 1..128 on LOOP/COMPOSED LOOP tracks with further constraints. |
| 1 | byte | Tracking: 0 SLICE on LOOP, NORMAL on other sample tracks; 1 CHNG PITCH. |
| 2 | byte | Filter type: 0 LPF, 1 BPF, 2 HPF, 3 BEF. |
| 3 | byte | MIDI receive channel: 0..15 means channels 1..16; 16 OFF. |
| 4..5 | u16be | Output routing; normal word values 0..9, as below. |
| 6 | byte | MIDI transmit channel: 0..15 means channels 1..16; 16 OFF. |
| 7 | byte | MIDI transmit note: 0..127; 60 displays C3. |
| 8 | byte | MAIN pad function code. |
| 9 | byte | MAIN knob function code. |
| 10..18 | 9 bytes | Sample-name storage; ordinary name setter copies eight bytes only. |
| 19 | byte | Note overlap: 0 MULTI, 1 SINGLE. |
| 20..21 | 2 bytes | Native save writes zero; the corresponding reader discards them. Historical meaning unspecified. |
| 22 | byte | Sample presence/channels: `0xff` absent, 0 mono, 1 stereo. |
| 23 | byte | LFO waveform: 0 SAW DOWN, 1 SAW UP, 2 TRIANGLE, 3 SQUARE. |
| 24..27 | u32be | Physical sample frame count. |
| 28..29 | u16be | Integer sample rate. |
| 30 | byte | Width selector: `0x00` 16-bit, `0x80` 8-bit. |
| 31 | byte | Sample-window analysis beat count, independent of pattern length. |
| 32..35 | u32be | Selected sample start, in frames. |
| 36..39 | u32be | Selected sample end, in frames; see SSP endpoint limits. |
| 40..41 | 2 bytes | Native save writes zero; the corresponding reader discards them. Historical meaning unspecified. |
| 42..43 | u16be | Saved sample reference tempo; normal scale is tenths of BPM. |
| 44..47 | u32be | Mono/left sample-memory start, in four-byte storage words. |
| 48..51 | u32be | Mono/left inclusive allocation end, in the same units. |
| 52..53 | u16be | Mono/left allocation-record reference. |
| 54..57 | u32be | Right sample-memory start, active for stereo. |
| 58..61 | u32be | Right inclusive allocation end, active for stereo. |
| 62..63 | u16be | Right allocation-record reference, active for stereo. |

The ninth name byte, embedded-NUL residue and trailing spaces must remain
intact. Do not require all nine name bytes to match SONGCONT or SSP NAME.
The width getter interprets `0x80` specially and otherwise reports 16-bit,
but that fallback is not a validity rule for arbitrary selector bytes.

### Output And MIDI Routing

| Routing word | Destination |
| --- | --- |
| 0 | STEREO OUT |
| 1..6 | AS 1 through AS 6, respectively |
| 7 | AS 1+2 |
| 8 | AS 3+4 |
| 9 | AS 5+6 |

The display uses the low byte, but the stored field is a complete BE16 word.
A nonzero high byte is not specified merely because the low byte names an
output. Loading without the alternate-output capability clears the routing
word. The separately stored effects-control routing copy is described under
[Effects Routing](su700-effects.md#routing-and-applicability); its synchronization
is not fully specified, so do not automatically copy one representation over
the other.

Receive-channel editing prevents collisions with other tracks; initialization
instead lets later duplicate channels win. Transmit channels and notes have
no corresponding uniqueness restriction. Off-domain channel bytes must not
be silently converted to OFF.

### MAIN Control Codes

Stored knob codes are not UI selection indices:

| Code | Function | Code | Function |
| --- | --- | --- | --- |
| `0x00` | NONE | `0x01` | LEVEL |
| `0x02` | PAN | `0x21` | PITCH |
| `0x07` | ATTACK | `0x08` | RELEASE |
| `0x2e` | LENGTH | `0x30` | TIMING |
| `0x32` | VELOCTY | `0x31` | GATE TM |
| `0x09` | SPEED | `0x0a` | AMPDPTH |
| `0x0b` | FILDPTH | `0x0c` | PITDPTH |
| `0x05` | HI GAIN | `0x18` | HI FRQ |
| `0x06` | LO GAIN | `0x16` | LO FRQ |
| `0x03` | CUTOFF | `0x04` | RESONA |
| `0x80` | EFFECT1 | `0x81` | EFFECT2 |
| `0x82` | EFFECT3 | | |

All named choices except LENGTH are selectable for sample tracks; LENGTH is
LOOP-only. AUDIO IN offers LEVEL, PAN and EFFECT1..3. MASTER additionally
offers the four EQ functions. NONE is representable but not a normal selectable
MAIN knob choice. Selection availability is separate from whether the current
tracking mode permits editing the selected parameter.

Pad codes are `0x00` NONE, `0x3c` PLAY, `0x12` ON/MUTE and `0x13` LOOPRST.
Loaded `0x3d` also displays PLAY; preserve it rather than replacing it with
the normally emitted `0x3c`. Sample tracks offer PLAY, ON/MUTE and LOOPRST;
AUDIO IN offers NONE/ON-MUTE, and MASTER also offers LOOPRST. ROLL has no
selectable MAIN pad assignment in this mapping; a zero conversion-table entry
does not establish a second meaning for stored zero.

### Tracking, Overlap And Analysis

CHNG PITCH follows song tempo relative to the saved sample reference. Changing
into this mode resets ordinary pitch to zero. LOOP pitch updates are inhibited
under nonzero tracking mode; the same restriction does not apply universally to
COMPOSED LOOP/FREE. Noncanonical tracking bytes must not be reduced to a Boolean:
some consumers test nonzero and others exactly one.

SINGLE terminates matching prior voices within a playing category; sequenced
and live playing can coexist. Playback tests exactly one. A nonzero display
fallback does not make value two a validated SINGLE encoding.

For present LOOP samples, initialization takes live loop length from analysis
byte 31, with explicit failure handling for zero. Other sample tracks retain
their separate pattern length. COMPOSED LOOP shortening is constrained by the
retained event boundary, not just the numeric 1..128 editor range.

For an ordinary valid selected window `D = end + 1 - start`, explicit LOOP
length editing derives a reference using:

```text
Q = floor(D / 75) + (D % 75 > 37 ? 1 : 0)
N = 8 * requested_beats * integer_sample_rate
reference = floor(N / Q) + (N % Q > floor(Q / 2) ? 1 : 0)
```

The accepted reference is 400..2999. A successful change updates loop length,
analysis count and reference together; failed geometry preparation restores
their prior values. The device arithmetic has limited-width divisors, so these
expressions do not define behavior for overflow, zero divisors or malformed
windows.

Automatic analysis uses different entry paths, an ordinary 500..1900 target
band and half/double or neighboring beat-count choices. A fresh path begins
with four beats; adjustment of existing analysis has different seeds and
selection rules. On one reanalysis failure path, non-LOOP tracks clear analysis
count/reference but retain pattern length; LOOP tracks restore prior values.
Thus a non-LOOP sample can retain zero analysis/reference alongside valid
audio. A live reference can fall back to 1200 without changing the saved zero.

Stored reference tempos are not a recalculation invariant of current windows.
Do not repair them by applying one formula or force byte 31 to equal byte 0.
Complete historical analysis and mode-transition behavior remain unspecified.

### Allocation References

Both channel triplets are serialized even for mono or absent samples. Inactive
right-channel bytes may retain unrelated state; they do not imply a missing
right SSP block. Allocation starts/ends are native sample-memory locations,
not disk sectors, file offsets or playback markers.

For the normal allocation-reference representation, `H` selects table entry
`(H - 0xe800) / 6`; this relationship does not define malformed or unaligned
references. Capacity is checked against allocation length, not necessarily
equality with the smallest possible allocation. No safe fresh-authoring policy
for these fields is specified. Preserve them and respect the
[bulk-load precedence](su700.md#cross-file-loading-and-completeness).

## Sample Scenes

`Tgsn` contains eight records without an enclosing size word. An absent record
is `ff ff`. A populated record contains forty 22-byte snapshots followed by
two mute bytes, totaling 882 bytes. The first word tested for absence belongs
to snapshot zero; no presence prefix is inserted before the snapshots.

For sample slot `n`, its snapshot begins at scene offset `22*n`:

| Offset | Type/size | Parameter | Automation opcode |
| --- | --- | --- | --- |
| 0 | byte | LEVEL | `0x01` |
| 1 | byte | PITCH; native encoding, not a MIDI-controller value | `0x21` |
| 2 | byte | PAN | `0x02` |
| 3 | byte | ATTACK | `0x07` |
| 4 | byte | RELEASE | `0x08` |
| 5 | i8 | GRV TIMING amount | `0x30` |
| 6 | byte | Shared groove resolution | High value byte of `0x30..0x32` |
| 7 | i8 | GRV GATETIME amount | `0x31` |
| 8 | i8 | GRV VELOCITY amount | `0x32` |
| 9 | byte | LFO SPEED | `0x09` |
| 10 | byte | LFO AMP DPTH | `0x0a` |
| 11 | byte | LFO FIL DPTH | `0x0b` |
| 12 | byte | LFO PIT DPTH | `0x0c` |
| 13 | byte | EQ HI GAIN | `0x05` |
| 14 | byte | EQ HI FRQ index | `0x18` |
| 15 | byte | EQ LO GAIN | `0x06` |
| 16 | byte | EQ LO FRQ index | `0x16` |
| 17 | byte | FILTR CUTOFF | `0x03` |
| 18 | byte | RESONANCE | `0x04` |
| 19 | byte | Sample mute; normal 0 on, 1 muted | `0x12` |
| 20..21 | u16be | SAMPLE LNGTH; LOOP-only application, complete domain/units unspecified | `0x2e` |

Scene offsets 880 and 881 store MASTER and AUDIO IN mute respectively.
Complete units/ranges and audible transformations are not specified for all
named controls. In particular, a name such as ATTACK does not establish a
linear millisecond encoding or identical behavior on all track families.

### Groove And EQ Encodings

Groove amounts are stored signed, normally -100..100, without an on-disk bias.
The shared resolution selector has ordinary editing values 1 thirty-second,
2 sixteenth, 3 eighth and 4 quarter note. Only the Timing control changes it
through normal resolution editing; gate/velocity events also carry and can
replace it. Computational handling exists for 5/6, but this does not establish
additional normal UI choices. Invalid selectors do not reset all derived state.

LOOP groove editing requires SLICE tracking. COMPOSED LOOP/FREE timing clamps
negative amounts to zero; a gate/velocity update can also cause that clamp
because it reapplies shared timing. Their own amounts retain the signed range.
Active groove velocity adds the signed amount and clamps the result to 1..127,
rather than multiplying by a percentage. Note-on and note-off timing adjustments
are paired and stateful; a generic independent swing formula is insufficient.

EQ frequency fields are indices. High EQ normally permits 28..58, low EQ 4..40.
Indices 4..58 select the following display labels in order:

```text
32   36   40   45   50   56   63   70   80   90
100  110  125  140  160  180  200  225  250  280
315  355  400  450  500  560  630  700  800  900
1.0k 1.1k 1.2k 1.4k 1.6k 1.8k 2.0k 2.2k 2.5k 2.8k
3.2k 3.6k 4.0k 4.5k 5.0k 5.6k 6.3k 7.0k 8.0k 9.0k
10k  11k  12k  14k  16k
```

Display rounding is not a specification of exact filter frequency. Do not apply
these labels to arbitrary out-of-domain indices.

### Scene Application And Mute

A sample snapshot beginning with `0xff`, an unloaded sample (`Trkp[22] == 0xff`)
or a protected edit target can be skipped during recall. An absent scene is
not equivalent to a populated zero-filled scene.

Recall first handles the effects scene, then MASTER mute, individual sample
mutes, AUDIO IN mute and the remaining sample parameters. MASTER can change
all sample mute states and AUDIO IN, so restoring it last would overwrite the
independent states preserved by the actual recall order. Mute does not erase
the saved LEVEL value.

Sample absolute mute application replaces bit 0; immediate toggle context
toggles it. Some consumers test the whole byte, so upper bits are not specified
as unused. Common mute fields can be loaded in full even though ordinary values
are 0/1. A nonzero display is not permission to author arbitrary mute bytes.

## Event Tracks

Each `Tr` record has a BE16 descriptor: high byte is kind, low byte is track
identity. Kinds and identities are distinct from MIDI channels and opcodes.

| Kind | Interpretation |
| --- | --- |
| 0 | Global event context. |
| 1 | MASTER event context. |
| 2 | AUDIO IN event context. |
| 3 | LOOP sample event context. |
| 4 | Generated sample-dependent stream; no event words stored in SSQ. |
| 5 | COMPOSED LOOP sample event context. |
| 6 | Repeating sample-event stream with repeat-relative time. |
| 7 | FREE sample event context. |

Generated kind-4 slice timing/sample-address pairs are reconstructed from
sample data and analysis. They are not ordinary SSQ event words or hidden
payload following a kind-4 descriptor. Their exact sample boundaries and full
scheduling cannot be reconstructed from the descriptor alone.

### Time Words

An ordinary event word consists of bytes `T F Vh Vl`, with opcode `T`, fractional
time byte `F` and BE16 value `Vh Vl`. A `0x7f` word with value `0xffff` ends
the stream irrespective of `F`; other `0x7f` words replace the coarse time
with the value word, starting from zero. Time words are absolute replacements,
not deltas.

For event ordering, position is `(coarse << 16) + (F << 8)`. One coarse unit is
a sixteenth note, so the nominal position in quarter notes is
`coarse / 4 + F / 1024`. For example, coarse four and fraction 128 gives
1.125 quarter notes. This stored grid differs from the ordinary 1/48-quarter
transport step; it does not specify complete playback scheduling.

Playback treats coarse values above `0x4000` as end conditions, broader than
the canonical serialized `0xffff` terminator. Preserve noncanonical words
rather than claiming that playback reaches every structurally decoded time.
The second byte of immediate commands or special event forms must not be
universally reinterpreted as a timestamp without context.

For repeating kind 6, the period is `4 * Trkp[0]` coarse units, or `Trkp[0]`
quarter-note beats, independent of meter. Repeat-relative comparison preserves
the fractional part, defers an event on equality, drains the previous cycle
on wrap and then rewinds. Zero length has no safe general interpretation.

### Values And Context

The sample automation opcodes are listed with the scene fields above. Other
established actions include:

| Opcode | Context | Value interpretation |
| --- | --- | --- |
| `0x12` | Sample/MASTER/AUDIO IN | Mute low byte, subject to absolute/toggle context. |
| `0x14` | LOOP/COMPOSED LOOP | Loop-length change with the constraints described above. |
| `0x15` | Sample | Tracking mode in low byte. |
| `0x19` | Sample | Output routing in low byte. |
| `0x1c` | Sample | Note-overlap assignment in low byte. |
| `0x2b` | Sample | MAIN pad code in high byte, knob code in low byte. |
| `0x30..0x32` | Sample | Shared groove resolution in high byte, signed amount in low byte. |
| `0x3c` | Sample | Voice trigger; MIDI output takes low byte as velocity and Trkp byte 7 as note number. |
| `0x3d` | Sample | Voice release; MIDI output uses that note number with zero velocity. |
| `0x49` | Global | Scene index in low byte, normal disk scenes 0..7. |
| `0x7b` | Global | Tempo word in tenths of BPM, with clock-dependent behavior. |
| `0x7e` | Global | Meter high byte = beats; low byte multiplied by six for common duration. |
| `0x7f` | Global/sample | Coarse-time replacement or end. |

An opcode's presence in an immediate command dispatcher does not mean every UI
operation records it into the song. Conversely, the table is not exhaustive;
unknown opcodes and value subfields must remain intact.

Global `0x3c/0x3d` handlers are no-ops. Native LOOP triggering uses a fixed
velocity in a path where MIDI still transmits the event low byte; native sample
loudness and MIDI velocity are not universally identical. The high note-value
byte has additional unresolved roles. Unmatched MIDI releases can be suppressed.
SAMPLE LNGTH `0x2e` applies only to LOOP and is a no-op in the COMPOSED LOOP/FREE
table.

Scene-to-MIDI conversion also differs from stored values: pitch sends
`((raw + 0x80) & 0xff) >> 1`; SAMPLE LNGTH sends its low byte plus `0x40`
with byte wrap. These are output transformations, not native parameter ranges.
Effect controls use their separate effect records and Comn assignments 40..42.

Live editing can mark notes with `0xbc/0xbd`, remove duplicates, pair closing
events and replace intervals. Those operations are not a general normalization
rule for saved words. Preserve original order, timing, unknown bits and values;
full event-value semantics and lossless conversion to MIDI remain incomplete.
