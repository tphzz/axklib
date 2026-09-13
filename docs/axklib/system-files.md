# System Files

SYSTEM and SYSTEM2 store partition-level sampler configuration separately from
Programs and Volumes. A normal Volume save does not contain this environment.
These records use the PRF3 type, but the type tag alone does not identify a
System File: pathname, exact record size and the inner signature also matter.

Offsets below are hexadecimal. **Body offsets** start after the `0x30`-byte
shared SFS record envelope. Add `0x30` for a logical-file offset. The parameter
area begins at body `0x20`; a parameter-area offset is therefore body offset
minus `0x20`. These offsets must not be substituted directly for MIDI transfer
offsets. Multi-byte numeric fields are big-endian unless stated otherwise.

## Framing And Model Differences

| Partition-relative path | Model | Logical file size | Body size | Parameter area |
| --- | --- | ---: | ---: | --- |
| `\PRF3\SYSTEM` | A3000 | `0x430` | `0x400` | Body `0x020..0x367`; `0x98` trailing bytes |
| `\PRF3\SYSTEM2` | A4000/A5000 | `0x1030` | `0x1000` | Body `0x020..0xfff` |

The shared envelope begins with `FSFSDEV3SPLXPRF3`. Each body begins with a
`0x20`-byte header. The following byte values describe the listed software
versions; version text is not an independent hardware-model identifier.

| Body offset | Size | SYSTEM | SYSTEM2 |
| --- | ---: | --- | --- |
| `0x00` | 4 | Signature `21 52 05 31` | Signature `de ad fa ce` |
| `0x04..0x07` | 4 | Unspecified | Unspecified |
| `0x08..0x0c` | 5 | Version text `0200A` for V2 | `0107A` for 1.07; `0150A` for 1.50 |
| `0x0d` | 1 | Unspecified | Unspecified |
| `0x0e` | 1 | Storage revision `0` | Storage revision `0` or `1` |
| `0x0f..0x1f` | 17 | Unspecified | Unspecified |

SYSTEM2 revision does **not** distinguish A4000 from A5000. Both revisions
store 32 Multi assignments; A4000 uses 16 parts and A5000 uses 32. A3000 has
neither the saved Program Mode nor the Multi Part table.

System Load does not apply every saved byte. A3000 applies body
`[0x020,0x368)`. A4000/A5000 software 1.07 applies `[0x020,0x664)` and 1.50
applies `[0x020,0x67c)`. The remaining saved bytes are not thereby padding or
free space. Preserve them. Software 1.07 accepts revision 0; 1.50 accepts
revision 1 and has a revision-0 conversion path. Its complete conversion and
hardware-dependent initialization rules are not specified here.

## Region Map

Ranges in this table are half-open; some storage roles overlap.

| Region | SYSTEM body | SYSTEM2 body |
| --- | --- | --- |
| Global preferences | `[0x030,0x05e)` | `[0x030,0x1f0)` |
| Effect favorites | `[0x070,0x0f0)` | `[0x200,0x300)` |
| Panel/command preferences | `[0x0f0,0x130)` | `[0x300,0x340)` |
| Recording effects and configuration | `[0x130,0x1ca)` | `[0x340,0x3f0)` |
| Registered Sample parameter block | `[0x1e8,0x2a4)` | `[0x40c,0x4ec)` |
| Registered Program region | `[0x2a4,0x368)` | `[0x4ec,0x64c)` |

Gaps between these regions include disk-selection and auxiliary state, not
automatically zero padding. In SYSTEM the disk cache at `[0x060,0x074)`
overlaps the first four favorite bytes. These are not independent storage
allocations. Remaining auxiliary MIDI meanings and SYSTEM2 hardware-dependent
bytes `0x672/0x673` are not fully specified.

## Global Preferences

This table uses offsets relative to body `0x030`. All fields are one byte
unless a range is given. Signed fields use two's complement.

| Relative offset | Meaning | Stored domain |
| --- | --- | --- |
| `0x00` | Master fine tune | s8, `-63..63` |
| `0x01`, `0x02` | Master coarse tune, transpose | s8, `-127..127` |
| `0x03` | Velocity curve selection | `0..17` |
| `0x04` | Basic Receive Channel | SYSTEM `0..15`; SYSTEM2 `0..31` |
| `0x05` | Stereo-to-assignable output selection | `0..5` |
| `0x06` | Receive, wave-edit and audition flags | See below |
| `0x07..0x0a` | Four knob transmit channels | s8, SYSTEM `-1..16`; SYSTEM2 `-1..32` |
| `0x0b..0x0e` | Four knob controller devices | `0..120` |
| `0x0f..0x14` | Six function-key transmit channels | SYSTEM `0..16`; SYSTEM2 `0..32` |
| `0x15..0x1a` | Six function-key notes | `0..127` |
| `0x1b..0x20` | Six function-key velocities | `1..127` |
| `0x21` | Stereo output-level offset selection | `0..4` |
| `0x22`, `0x23` | Total EQ low-boost frequency and gain | Frequency `4..40`; gain `52..76` |
| `0x24` | Remix type / variation | High / low nibble |
| `0x25..0x27` | Total EQ low frequency/gain/width | `4..40`, `52..76`, `10..120` |
| `0x28..0x2a` | Total EQ mid frequency/gain/width | `4..58`, `52..76`, `10..120` |
| `0x2b..0x2d` | Total EQ high frequency/gain/width | `28..58`, `52..76`, `10..120` |
| `0x2e` | SYSTEM2 Program Mode | `0` Single, `1` Multi |
| `0x2f` | SYSTEM2 additional flags | Bit 0 Remix auto audition; bit 1 knob MIDI out; bit 2 function-key MIDI out |
| `0x30..0x4f` | SYSTEM2 Multi Part Programs | Each `1..128` |
| `0x1b8`, `0x1b9` | SYSTEM2 Remix zone start/end | `0..7`, `1..8`; start less than end |
| `0x1ba..0x1be` | SYSTEM2 five assignable output offsets | Each `0..4` |
| `0x1bf` | SYSTEM2 working-Program marker | Transient selection state, cleared on load |

Basic Receive uses `0..15` for A01..A16 and `16..31` for B01..B16. Transmit
selection `16` means Basic Receive; SYSTEM2 transmit selections `17..32` mean
B01..B16. Knob transmit selection `-1` means audition. The Multi table stores
direct Program numbers, not the MIDI transfer representation `0..127`. The
part matching Basic Receive is the master part. Multi part channels override
the Sample/Bank Rch Assign settings inside the selected Programs.

Global flag byte `0x06` assigns bits 0..7 to Omni, Program Change Enable,
wave-length lock, auto zero, auto snap, audition with Easy Edit, audition with
effects, and Play & Load. Mode selection clears Omni. Length lock, auto zero
and auto snap are mutually exclusive edit options. The assignable output
duplicating Stereo Out has no independently effective level offset.

Total EQ gain selection `64` is neutral; low boost has no width field.
Output offsets and frequency selections are encoded choices, not literal dB
or Hz values. The MIDI-out bits in SYSTEM2 byte `0x2f` are load-sensitive;
their stored presence does not establish a durable enabled setting. Preserve
unrelated bits instead of inventing initialization values.

SYSTEM Remix type/variation domains are `0..4` / `0..3`; SYSTEM2 domains are
`0..9` / `0..7`. SYSTEM2 stores five registered recipes as three arrays:
duration codes at relative `0x50`, captured random choices at `0xc8`, and
processing flags at `0x140`. Each array has five consecutive 24-byte slots.
Keep slot order and unused tails. Registered recall, Gate state and arbitrary
source-loop bounds do not yet have complete independent construction rules.

## Recording Settings

Each recording region starts with three `0x28`-byte effect blocks. Their layout
matches the [Program effect blocks](sampler-data.md#program-effect-blocks):
SYSTEM uses type byte 7, SYSTEM2 uses type byte 6, and sixteen u16be words
begin at byte 8. There are three recording effects on every model, including
A5000. Ordinary effect types are `0..54` on A3000 and `0..96` on A4000/A5000.
Output destinations are `0..5` on A3000/A4000 and `0..8` on A5000.
Type changes replace the complete generation-specific parameter vector;
bypass does not reset it. Unused words are not independent controls.

Configuration begins `0x78` bytes into the recording region: body `0x1a8`
for SYSTEM, `0x3b8` for SYSTEM2. Offsets below are configuration-relative.

| Offset | Meaning | Encoding/domain |
| --- | --- | --- |
| `0x00` | Record type | `0` Replc, `1` New, `2` New+; SYSTEM2 also `3` Save |
| `0x01`, `0x02` | Mono/stereo, input selection | `0..1`, `0..4` |
| `0x03`, `0x04` | Frequency, pre-trigger selections | `0..6`, `0..5` |
| `0x05`, `0x06` | Start/stop trigger selections | Each `0..1` |
| `0x07`, `0x08` | Start/stop edge level | Each `0..63` |
| `0x09` | Map destination | `0..2`, dependent on record type |
| `0x0a` | Low key | s8 `-1..127`; `-1` is original key |
| `0x0b` | High key | u8 `0..128`; `128` is original key |
| `0x0c`, `0x0d` | Original key, auto normalize | `0..127`, `0..1` |
| `0x0e` | External SCSI ID | s8 `-1..7` |
| `0x0f`, `0x10` | External CD track/index | SYSTEM `1..255`; SYSTEM2 `1..99` |
| `0x11`, `0x12` | Monitor output/level | `0..5`, `0..127` |
| `0x13` | Click level | `0..127` |
| `0x14..0x15` | Click tempo | u16be hundredths, `8000..15999` |
| `0x16` | Click beat | `1..15` |
| `0x17`, `0x18` | Monitor enable, map auto | Each `0..1` |
| `0x19`, `0x1a` | Map original key, map all keys | `0..127`, `0..1` |
| `0x1b..0x22` | SYSTEM2 destination disk cache | 8 bytes |
| `0x23..0x32` | SYSTEM2 destination volume cache | 16 bytes |
| `0x33` | SYSTEM2 A/D input gain selection | `0..1` |

For inputs `0..2`, frequency selections display as `44.1k`, `22k`, `22kLoFi`,
`11k`, `11kLoFi`, `5k`, `5kLoFi`. For digital/optical inputs `3..4`, choices
`0..3` instead mean `ext`, `ext/2`, `ext/4`, `ext/8`. Display strings are not
exact sample-rate measurements. Selecting digital/optical input forces stereo,
caps the frequency choice at 3 and resets monitor output to zero. Monitor level
is not an independent setting in those input modes.

New limits map destination to `0..1`; New+ allows `0..2`. Replc and Save do
not offer independent destination-map editing. Effective low key must not exceed
effective high key after original-key substitution. CD track/index limits for
an actual operation depend on the inserted disc. Destination cache bytes are
snapshots, not a reliable identifier of a currently selected recording target.

## Effect Favorites

Each effect row occupies two bytes and stores four zero-based parameter
selections: high nibble then low nibble of the first byte, followed by high
then low nibble of the second. SYSTEM uses raw effect-ID order for 55 ordinary
effects; SYSTEM2 uses displayed effect-number order for 97 ordinary effects.
The complete areas occupy 128 and 256 bytes respectively, including dormant
rows. These ordering schemes must not be interchanged.

A selection identifies a visible effect parameter. Repeated selections are
legal. SYSTEM offers at most the number of visible parameters, capped at four
positions; SYSTEM2 offers four positions for nonempty effects. Through has no
editable favorites. Unavailable selections and dormant nibbles are retained
state, not instructions to reset other favorites or effect parameter words.

## Panel And Command Preferences

Each panel area is 64 bytes. The table uses **decimal byte indices** relative
to body `0xf0` for SYSTEM and `0x300` for SYSTEM2. Choices are stored indices,
not enum values from a software interface.

| Index | Meaning | SYSTEM | SYSTEM2 |
| ---: | --- | --- | --- |
| 0 | Format drive selection | `0..7` | `0..9` |
| 1 | Effect edit mode | `0` Full, `1` Favorite | Same |
| 2..5 | Knobs 2..5 control types | `0` Off, `1` On, `2..4` Step 1..3 | Same |
| 6 | Assignable Key | `0` Knob Control, `1` Damp, `2` Controller Reset, `3` Function Key Play, `4` Knob & Function Key, `5` MIDI to Sample | Same |
| 7 | Audition trigger | `0` Normal, `1` Toggle | Same |
| 8 | Function selection | `0` First, `1` Last, `2` Hold | Same |
| 9 | Page selection | `0` First, `1` Last | Same |
| 10 | Note display | `0` Name, `1` Number | Same |
| 11 | Format type | `0..2` Quick/2HD/2DD | `0..5` Logical/Physical/OnePartition/Quick/2HD/2DD |
| 12 | Sample name ordering | `0` no name sort, `1` forward, `2` backward | Unspecified |
| 13 | Program-on placement | `0` top, `1` mixed | Unspecified |
| 14 | Bank member visibility | `0` hide, `1` show | Unspecified |
| 15 | End display coordinates | `0` Address, `1` Length, `2` Time, `3` Beat, `4` Graph | `0..3`, no Graph |
| 16 | Import view | Unspecified | `0` All, `1` Sample Bank, `2` Sample, `3` Sequence |
| 17 | Knob 1 type | `0` Page, `1` Sample | Same |
| 18 | Audition NameView | `0` enabled, `1` disabled | Semantics incomplete |
| 19 | Layer-selection scope | `0` all pages, `1` selection page | `0` all pages, `1` Tree page |
| 20 | MIDI-to-Sample NameView | `0` enabled, `1` disabled | Unspecified |
| 21 | Unspecified | Preserve | Preserve |
| 22 | Sample sort | Unspecified | `0` Off, `1` Name, `2` Rch&Name |
| 23, 24 | Tree/Bank sort | Unspecified | `0` Off, `1` Name, `2` Status&Name |
| 25 | CD-R SCSI ID | Unspecified | `0..7` |
| 26 | CD-R write speed | Unspecified | `0..4` mean x1/x2/x4/x6/x8 |
| 27 | Service state | Unspecified | Test Mode popup type; not an ordinary preference |
| 28..63 | Remaining storage | Semantics incomplete | Semantics incomplete |

These bytes store selections, not commands: changing a Format choice does not
format a disk; changing Import view or CD-R speed does not start either operation.
End display coordinates are not a wave boundary or loop mode. SYSTEM NameView
booleans have inverted storage polarity. Do not transfer meanings between
generations simply because byte indices coincide.

A3000 name sorting compares all 16 unsigned name bytes without trimming or
case folding. Bank-before-Sample grouping is independent of sort direction;
Program-on top additionally prioritizes assigned objects. Equal-name ordering
is not guaranteed stable. InBank hide affects the selection list, not storage.

## Registered Sample And Program Templates

The registered Sample contains a `0xbc`-byte A3000 or `0xe0`-byte A4000/A5000
parameter block, not a complete SBNK object. Both member lanes, bitmaps, flags,
EQ coefficients and endpoint caches are stored. A stored second lane does not
alone establish active stereo playback or supply a live Wave Data length.

The current block uses the [SBNK parameter window](sampler-data.md#sample-parameter-window)
relative to its `0x0a8` base. A3000 output groups instead occupy block offsets
`0xa5/0xa7` with destination domains `0..4/0..5`; current groups occupy
`0xd6/0xd8` with `0..12`. Native bend/coarse-tune domains are `0..13` and
`-127..127`; AEG attack mode is `0..1`, versus current `0..2`. Do not invent
current EQ-type, extended velocity-crossfade or portamento fields in the native
block. EQ gain is stored dB plus 64; displayed LFO speed is stored value plus 1.

Registered Program storage is also rearranged, not an ordinary PROG payload:

| SYSTEM body | SYSTEM2 body | Contents |
| --- | --- | --- |
| `0x2a4..0x31b` | `0x4ec..0x563` | Three 40-byte effect blocks |
| `0x31c..0x32b` | `0x564..0x573` | Four legacy controller records |
| Absent | `0x574..0x5eb` | Effect blocks 4..6 |
| Absent | `0x5ec..0x5fb` | Four current controller records |
| Absent | `0x5fc..0x623` | Port-B maps, extra effect connection, right/extended A/D, StepWave and retained suffix |
| `0x32c..0x333` | `0x624..0x62b` | Retained common prefix |
| `0x334..0x349` | `0x62c..0x641` | Common effect/A-D/LFO, level, transpose and portamento fields |
| `0x34a..0x367` | `0x642..0x64b` | Suffix with incomplete semantics |

Common fields and effects follow their generation's parameter encoding, but
controller ownership and topology-dependent update rules are not fully
specified for every registered block. A3000 has shared A/D routing, not an
independent right route. SYSTEM2 retains six effects and port-B state regardless
of whether the saving device exposes every setting.

## Unspecified State And Modification Limits

Header gaps, auxiliary state, native panel tails, parts of registered templates
and the unused-by-load suffixes have incomplete semantics. A stored value outside
a listed domain is not a default, and an unnamed byte is not free space.
Preserve unrelated bytes and do not use saved runtime pointers as persistent
identities. Independently constructing a complete System File requires more
initialization rules than this specification currently supplies.

For axklib's supported operations, see [Writer And Alteration](write.md).
