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

The version text occupies exactly five bytes; `0x0d` is not specified as a
required string terminator. A reset does not establish complete initial values
for the unspecified header bytes. Preserve them when modifying an existing file.

SYSTEM2 revision does **not** distinguish A4000 from A5000. Both revisions
store 32 Multi assignments; A4000 uses 16 parts and A5000 uses 32. A3000 has
neither the saved Program Mode nor the Multi Part table.

System Load does not apply every saved byte. A3000 applies body
`[0x020,0x368)`. A4000/A5000 software 1.07 applies `[0x020,0x664)` and 1.50
applies `[0x020,0x67c)`. The remaining saved bytes are not thereby padding or
free space. Preserve them. Software 1.07 accepts revision 0; 1.50 accepts
revision 1 and has a revision-0 conversion path. Its complete conversion and
hardware-dependent initialization rules are not specified here.

The header is read separately. A3000 then reads body `[0x020,0x368)`;
A4000/A5000 reads `[0x020,0x1000)` before applying its shorter prefix.
A short read within a requested range aborts parameter application. This does
not establish the missing contents of a truncated saved suffix or permit
reconstructing them with zeros.

System Bulk MIDI transfer has different framing from a disk file: payload
bytes are sent as high and low nibbles, without the System header. A3000 V2
transmits body `[0x020,0x368)`. A4000/A5000 software 1.07 and 1.50 transmit
`[0x020,0x1000)`, including the saved suffix, but receive application covers
only the shorter prefix listed above for each version. Inclusion in a transfer
does not make unspecified bytes editable settings or establish their defaults.

## Region Map

Ranges in this table are half-open; some storage roles overlap.

| Region | SYSTEM body | SYSTEM2 body |
| --- | --- | --- |
| Global preferences | `[0x030,0x05e)` | `[0x030,0x1f0)` |
| Disk configuration transfer | `[0x060,0x074)` | `[0x1f0,0x200)` |
| Effect favorites | `[0x070,0x0f0)` | `[0x200,0x300)` |
| Panel/command preferences | `[0x0f0,0x130)` | `[0x300,0x340)` |
| Recording effects and configuration | `[0x130,0x1ca)` | `[0x340,0x3f0)` |
| Service startup state | `[0x1d0,0x1d8)` | `[0x3f4,0x3fc)` |
| Auxiliary MIDI setup | `[0x1d8,0x1e0)` | `[0x3fc,0x405)` |
| Registered Sample parameter block | `[0x1e8,0x2a4)` | `[0x40c,0x4ec)` |
| Registered Program region | `[0x2a4,0x368)` | `[0x4ec,0x64c)` |
| Sequence MIDI-port configuration | Not present | `[0x64c,0x65c)` |
| Digital-output configuration | Not present | `[0x65c,0x664)` |
| mLAN configuration and initialization state, software 1.50 | Not present | `[0x664,0x674)` |

Gaps between these regions include disk-selection and auxiliary state, not
automatically zero padding. In SYSTEM the disk cache at `[0x060,0x074)`
overlaps the first four favorite bytes. These are not independent storage
allocations. Remaining auxiliary MIDI meanings and complete mLAN initialization
rules are not fully specified.

In both layouts, body `0x020` and `[0x029,0x030)` have no specified individual
meaning or required initial value. The neighboring disk-state reset changes
only `0x021`, `0x022`, `0x023`, `0x024` and `0x025`; it does not clear these
eight unspecified bytes. Preserve them during unrelated edits. Their inclusion
in a complete configuration transfer does not make them editable settings.

### Unspecified Intervals

The following intervals have no specified per-byte parameter meaning. Retain
them exactly when editing neighboring settings; they are not alignment padding.

| Region | SYSTEM body | SYSTEM2 body | Modification constraint |
| --- | --- | --- | --- |
| Global/disk gap | `[0x05e,0x060)` | Not present | Outside both adjacent parameter blocks |
| Recording configuration tail | `[0x1c3,0x1ca)` | `[0x3ec,0x3f0)` | Inside the recording block; reset clears these bytes, but nonzero retained values must not be normalized |
| Recording/service gap | `[0x1ca,0x1d0)` | `[0x3f0,0x3f4)` | Outside recording and service blocks |
| MIDI/registered-state gap | `[0x1e0,0x1e8)` | `[0x405,0x40c)` | Not extra auxiliary MIDI selectors |
| Registered-state tail | `[0x34a,0x368)` | `[0x642,0x64c)` | Included in the shared registered-state load, outside the registered Sample/Program parameter copies |
| Applied extension remainder, software 1.50 | Not present | `[0x674,0x67c)` | Applied by System Load, but outside the 16-byte mLAN block; meaning and initial values unspecified |

SYSTEM's saved-only suffix is `[0x368,0x400)`. SYSTEM2 software 1.07's
non-applied suffix begins at `0x664`, and 1.50's begins at `0x67c`; both end
at `0x1000`. These suffixes remain part of the saved file even though normal
System Load does not apply them. Their internal roles and complete initial
values are unspecified. Neither a larger save/transfer size nor a neighboring
block's reset values authorize manufacturing or truncating them.

### Service Startup State

The first byte at SYSTEM `0x1d0` / SYSTEM2 `0x3f4` is a service/test startup
request. Zero is inactive; values `1` and `2` request test-entry modes whose exact
mode names are unspecified. This is not a normal user preference. Reset clears
the complete eight-byte block; System Load clears a nonzero first byte. The
other seven bytes have no specified individual meaning. Preserve the block
when modifying unrelated settings; do not use it to author service requests
or apply load-time normalization to a lossless file rewrite.

## Sequence Port And Digital Output

These fields have the same encoding in both SYSTEM2 revisions and are not
present in the A3000 SYSTEM layout.

| Body offset | Size | Setting | Stored values |
| --- | ---: | --- | --- |
| `0x64c` | 1 | Sequence recording port and playback channel group | 0 = B, 1 = A |
| `0x65c` | 1 | Digital coaxial/optical output bit depth | 0 = 20 bits, 1 = 24 bits |

Port A records MIDI IN-A and plays through channels A01..A16; port B uses
MIDI IN-B and B01..B16. A4000 has no second MIDI input or editable port choice
and uses the A value on load. Storage revision does not identify that hardware
capability. The digital bit-depth choice is 20/24, not 16/24, and does not alter
stored Wave Data resolution.

Reset clears the 16-byte sequence block and sets its first byte to 1; it clears
the eight-byte digital block to 0. System Load converts a digital selector above 1
to 0 in working memory, without rewriting the saved byte. An unrelated file
edit must preserve the original saved values rather than apply these load-time
adjustments.

The remaining 15 sequence bytes and 7 digital bytes have no specified individual
meaning. Preserve them when editing either scalar. Their block reset values
do not establish a complete set of defaults for constructing a new System File.

## Disk Preferences

| Setting | SYSTEM body | SYSTEM2 body | Encoding |
| --- | --- | --- | --- |
| Self ID | `0x60` | `0x1f0` | SCSI ID 0..7 |
| Mount selections | `0x61`, byte | `0x1f4`, BE32 | Set bit selects a mounted device |
| Top Partition | `0x68` | `0x1fc` | Stored zero-based; display adds1 |

Mount bits 0..7 select SCSI IDs 0..7. SYSTEM2 adds bit 8 for IDE master and bit 9
for IDE slave. Preserve its remaining bits. The sampler cannot mount its own
SCSI ID. System Load masks Self ID to three bits and clears the corresponding
mount bit. When changing saved ID or mount selections, clear the final own-ID
bit and reject an explicit request to mount that ID; retain other selections.
An unrelated edit must not normalize these fields. A saved ID change does not
assert that the ID is unused on the connected bus or change live hardware.

Top Partition selects the beginning of an eight-partition AKAI import window,
not an SFS partition-allocation limit. The starting index is adjusted locally
when fewer than eight partitions remain; this does not rewrite the saved
selection. The sampler editor permits displayed 1..99 (stored 0..98), but System
Load resets stored values above 97 to 0. Consequently displayed 99 is not a
load-stable saved selection; use 1..98 for edits intended to survive loading.
Retain an existing 99 or other unrequested raw value rather than repairing it
as a side effect of an unrelated edit.

### Random State

SYSTEM body `0x64..0x67` and SYSTEM2 body `0x1f8..0x1fb` contain a big-endian
32-bit pseudorandom seed accumulator. Loading supplies it as a generator seed;
disk activity adds generated values to the cached accumulator, which is later
included in disk-configuration saves. It is not a checksum or a direct snapshot
of the generator's current internal state.

The separate big-endian 16-bit value at body `0x26..0x27` supplies another disk
operation seed and is incremented after use. The generator algorithms and
whether these seed sources share a generator differ between generations.
Neither value is an audio parameter or a stable file identifier. Preserve both
during unrelated edits rather than advancing, synchronizing or zeroing them.
The accumulator can nevertheless contribute to generated names. Canonical
initial seed values remain unspecified.

### Disk Conflict State

In A3000 V2 SYSTEM and SYSTEM2 software1.07/1.50, body `0x28` stores the One/All scope of same-name
conflict dialogs: zero means One, nonzero means All, and interactive changes
store0 or1. Disk-command entry resets it to One. This byte is separate from
the selected Rename, Skip, Replace or Abort action and does not establish a
persistent Replace All default.

Body `0x23` and `0x24` supply the initial values for load and save conflict-policy
caches respectively. Values 0/1/3/4/5 select asking, renaming, skipping, replacing and aborting,
subject to the object and conflict type. Value2 has a separate acceptance path
whose full meaning is unspecified. Values above5 are read as0 without necessarily
rewriting storage. An All decision can update a working cache without changing
its saved initial value. Value2 is accepted for a System-file overwrite but does
not have an ordinary conflict-dialog action; its intended use remains unspecified.

Preserve these bytes during unrelated preference edits. They are command state,
not musical parameters. Body `0x21` has boolean access, `0x25` has byte access,
and `0x22` is cleared with the prefix reset, but their operational meanings remain
unspecified. They must not be treated as freely disposable padding.

## Auxiliary MIDI Setup

SYSTEM stores eight bytes at `0x1d8`; SYSTEM2 stores nine at `0x3fc`.
Native A3000 V2 and SYSTEM2 software1.50 share the receive-filter and timeout
roles described below, but differ in how loading applies the saved filters.
The five ordinary preferences have the same storage domains in both formats:
SYSTEM uses `0x1da..0x1dd` for protection and the three receive-disable flags,
and `0x1df` for device number. Its additional Program Change filter is at
`0x1d8`, unspecified setting at `0x1d9`, and timeout recovery bypass at `0x1de`.
SYSTEM has no saved receive-port selector.

| SYSTEM2 body offset | Meaning | Stored domain |
| --- | --- | --- |
| `0x3fc` | Additional incoming Program Change filter | `0` receive, `1` filter |
| `0x3fd` | Unspecified setting | `0..15`; preserve its value |
| `0x3fe` | Bulk Protect | `0` off, `1` on |
| `0x3ff` | Channel Aftertouch reception | `0` receive, `1` disable |
| `0x400` | Control Change reception | `0` receive, `1` disable |
| `0x401` | Pitch Bend reception | `0` receive, `1` disable |
| `0x402` | Internal Active Sensing timeout recovery bypass | Boolean; see below |
| `0x403` | MIDI device number | `0` off, `1..16` numbered, `17` all |
| `0x404` | SysEx receive port | `0` A, `1` B; A4000 uses A |

The additional Program Change filter is separate from the global Program
Change preference. Do not collapse them into one stored field. The internal
byte at `0x402` bypasses subsequent timeout recovery when set, but does not
disable Active Sensing monitoring or prevent the receive queue from being
reset first. Its intended user-facing use remains unspecified; it is not a
general "Active Sensing off" switch.

Loading normalizes invalid booleans and the unspecified `0x3fd` setting to
zero, and invalid device numbers to17. Single-port hardware applies port A
regardless of the saved port byte. Those working-state rules do not authorize
rewriting unrelated bytes during a lossless copy or a targeted edit. Protection,
the three ordinary receive-disable flags, device number and an available SysEx
port can be changed independently without altering the other saved bytes.
Preserve relative bytes0,1 and6 on these edits. A3000's MIDI settings load step
restores and validates these saved values without applying the four live
receive filters. Their subsequent activation timing is unspecified; changing
a saved value is distinct from applying a live control change. Internal-switch use and
complete-file initialization remain separate limitations.

## mLAN Extension

Software 1.50 uses body `[0x664,0x674)` for mLAN configuration and retained
interface state. Applicability depends on interface availability; a SYSTEM2
file does not prove that the receiving device has that interface attached.
These selections are distinct from Basic Receive Channel and Program routing.
Changing either saved input selection does not require changing the other
selection or the retained initialization state. Preserve the remaining bytes
of this block on such an edit. MIDI source `2` requires dual-port hardware.
These routing fields belong to storage revision `1`; revision `0` does not
apply this extension. An out-of-domain input selection does not establish a
default and need not be changed when editing the other input selection.

The mLAN page's Output Level Offset, Word Clock Mode and Nickname are separate
interface settings, not fields in this System block. Output Level Offset has
five choices, `+0`, `+6`, `+12`, `+18` and `+24 dB`. Word Clock includes live
internal/external status, and Nickname is a ten-byte interface name. Do not
place these values in the unspecified bytes at `0x666..0x671`.

| Body offset | Meaning | Stored values or constraint |
| --- | --- | --- |
| `0x664` | MIDI input source | `0` MIDI connectors; `1` mLAN A; `2` mLAN B |
| `0x665` | Audio input source | `0` ADIn; `1` mLAN |
| `0x666..0x671` | Unspecified interior state | Preserve on unrelated edits |
| `0x672` | Interface initialization-completed marker | Zero requires initialization; successful initialization sets `1` |
| `0x673` | Retained initialization routing and flags | See below; not an ordinary preference |

On single-MIDI-port hardware, MIDI source `1` is displayed as mLAN rather than
mLAN A, and a loaded selection `2` is applied as `1`. This does not change the
meaning of the saved byte or authorize normalizing it during a lossless copy.

When initialization is needed, bit 0 of `0x673` selects routing value `1`;
otherwise bit 1 selects routing value `2`. If neither is set, initialization
fails. Bit 0 takes precedence when both are set. Diagnostic preparation stores
`0x81` or `0x82` with completion zero after its successful selected-port check.
Bit 7 marks that preparation; its wider meaning is unspecified, and it is not
an ordinary on/off preference. Bits 2..6 have no defined independent setting.
Preserve them rather than treating them as reserved-zero fields.

Interactive initialization retains the interface's cached nickname; pending
startup initialization supplies the model name as the initial nickname.
Initialization also provisions vendor/model and audio/MIDI endpoint labels
in the external interface. These strings are not stored in `0x666..0x671`.
The completion byte is set only after the external command sequence succeeds;
the other retained bytes are not a substitute for the interface's live identity
or its command responses.

Reset of this block clears its first 14 bytes but retains `0x672/0x673`.
The retained marker does not establish initialization on another device.
Do not synthesize it as `1` to bypass initialization or assume that an
all-zero block is a valid fresh configuration. The interior bytes retain
unspecified meanings; portable initialization cannot be derived from this
block without the target interface's state and initialization contract.

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

The saved SYSTEM2 global block ends at relative `0x1c0` (body `0x1f0`).
Resolved Program references are runtime state, not additional fields following
this block. Preserve the subsequent disk-settings region as a separate section;
do not append or serialize runtime pointers there.

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
Keep slot order and unused tails. Duration zero terminates a recipe; a zero
first duration leaves the source loop unchanged. Nonempty lanes must terminate
within their 24 bytes before being used to construct a new recipe.

For a source loop of `N` frames, let `E = floor(N/8)`. Duration codes `1`, `2`,
`9` and `8` consume `E`, `2*E`, `floor(N/16)` and `floor(N/32)` frames,
respectively. The full Remix zone covers `8*E` frames. Short-slice patterns can
produce fewer frames than that zone and leave part of its result dependent on
previous working-buffer contents. A nominal total of one loop is therefore
not enough to guarantee a fully defined result for every loop length.
Patterns made only of `1` and `2` with `count(1)+2*count(2)=8` fill that zone
for every length. Stereo source coordinates and Gate processing have additional
dependencies; this arithmetic alone is not a complete construction rule.

The generated processing bytes have these meanings:

| Value (decimal) | Operation |
| --- | --- |
| `0` | Copy the corresponding source segment |
| `1` | Copy a captured random source slice |
| `3` | Reverse a captured random source slice |
| `5` | Copy from the source position `2*E` frames after loop start |
| `6` | Silence |
| `11` | LoFi processing of a captured random source slice |
| `19` | Pitch processing of a captured random source slice |
| `35` | Gate; depends on working descriptor state not stored in the recipe |

For duration `2`, the low two random-choice bits select a quarter-sized source
slice; otherwise the low three bits select an eighth start. LoFi and Pitch
also use random bit 0 for their processing variant. Keep the whole captured
byte. Copy, fixed-source and silence steps capture zero rather than a random
choice. Processing bytes are complete operations, not freely combinable flags.

A replacement made only from durations `1`/`2` totaling eight eighths and the non-Gate
operations above has no short-slice rounding gap. It has at most eight active
steps, followed by zero in all three lanes through the end of that slot.
An empty replacement clears the three lanes. Replacing one slot must not
alter the other slots or normalize their unused tails. These storage rules
do not validate future source capacity. Stereo execution requires matching
active loop windows: generation uses the primary window for both lanes,
while publication uses each lane's own window.

Stored type selections `5..9` refer to User1..User5, but types `8`/`9` do not survive
loading: the complete selection byte is reset. This affects the saved selection,
not the contents of the fourth and fifth recipe slots. Gate and short-slice
recipes retain dependencies on working state or source geometry; the file
does not carry that missing context.

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

Ordinary Sample allocation applies the registered parameters but clears the
new Sample's assignment bitmap at block `0x18..0x27` and flags at `0x28`.
This does not erase those bytes from the saved template. Preserve them during
unrelated template edits; they are not free padding.

Applying a template is not a general recalculation of its EQ coefficients or
window caches. Later window repair can be selective, so it must not be relied
on to make an inconsistent template valid. Keep dependent values consistent
when changing parameters; do not assume a future load will repair them.

The current block uses the [SBNK parameter window](sampler-data.md#sample-parameter-window)
relative to its `0x0a8` base. A3000 output groups instead occupy block offsets
`0xa5/0xa7` with destination domains `0..4/0..5`; current groups occupy
`0xd6/0xd8` with `0..12`. Native bend/coarse-tune domains are `0..13` and
`-127..127`; AEG attack mode is `0..1`, versus current `0..2`. Do not invent
current EQ-type, independent velocity-crossfade widths or portamento rate/time fields in the native
block. A3000 EQ uses Peak/Dip behavior, with the same coefficient calculation
as current Peak/Dip; it does not supply the current selectable shelf types.
EQ gain is stored dB plus 64; displayed LFO speed is stored value plus 1.

In the A3000 block, MAP/OUT byte `0x29` bit0 enables Program-controlled
portamento and bit3 enables velocity crossfade. Both are independent switches;
changing either preserves all other bits, pitch words and wave/loop coordinates.
The native block has no per-Sample portamento rate/time or independent low/high
crossfade widths. Conversion to the current layout maps bit0 to portamento type
`0` Off or `1` Program and bit3 to both crossfade widths `0` or `5`.
This conversion rule does not add those extended fields to a native block.

The six A3000 Sample controllers use the prefix at block `0x00..0x17`,
with device `0..125` and function `0..21`. Current controllers use
`0xbc..0xd3`, with device `0..126` and function `0..36`. On a current
record change, copy all four bytes to its prefix record and replace a prefix
function above21 with zero. Unchanged records can retain unequal copies.
Unlike Program A/D routing, current Sample output edits do not project into
the legacy output groups. Current portamento type at `0xda` also controls
MAP/OUT bit0: set it for type1 and clear it for other types.

EQ edits update all five coefficient words from the effective frequency,
gain and width, plus the current type where present. Unrelated edits preserve
the stored vector. A3000 Peak/Dip calculation does not consume or clear the
unassigned current-type bits. Velocity ranges permit equal endpoints. Filter
and level scaling breakpoint1 must be strictly below breakpoint2 when editing
either point.

Loop mode at block `0x3d` is an independent value in `0..5`, using the same
six modes as a Sample. Changing mode does not move wave or loop coordinates
or recalculate their cached endpoints. It does not repair pre-existing invalid
windows; those bytes remain separate from the mode setting.

In both generations, root-key, sample-rate and fine-tune edits mirror the
selected value to the second stored lane. Pitch-word updates additionally
depend on actual Wave Data attachment: only attached lanes have their pitch
words recalculated. The calculation uses each lane's stored root, rate and fine
tune; attachment is not stored in the registered parameter block. Identical
registered parameters can therefore require different pitch-word updates.
Neither retained second-lane values nor the topology byte establishes actual
bindings. A root/fine change cannot determine all dependent pitch values from
the isolated template alone; the intended Wave Data lanes and their later
initialization also matter. Special key
endpoints are root-relative: when high key is `0x80`, an ordinary low key above
root must be reduced to root; when low key is `0xff`, an ordinary high key below
root must be raised to root. Window edits can change both lanes and the cached
ends at block `0xb4/0xb8`. Length lock determines which endpoint or length is
held constant during a start/end edit. These coupled fields must not be treated
as independent scalar values in an isolated template.

Wave-window bounds depend on the attached Wave Data capacity. Loop-window
bounds instead use the retained primary wave interval: loop start must be at
or after wave start and loop end at or before wave end. Zero-length loops at
an endpoint are representable. Explicit loop intervals can be changed without
attachment information when the retained primary wave start, length and cached
end are consistent and do not overflow. This does not establish that the
retained wave interval fits an absent or future source.

A complete loop interval consists of start and length. Changing it mirrors
start to `0x50/0x54`, length to `0x58/0x5c`, and writes their sum to the
loop-end cache at `0xb8`. It does not change the wave interval, pitch or topology.
For an explicit interval change, a retained unmodified start or length is a
coordinate of that interval; Length Lock governs panel endpoint gestures, not
the representation of an explicitly supplied interval.

Expand Detune at block `0x6a` and Dephase at `0x6b` can alter the expanded-mono
state at `0x28` bit2. When no right member is bound and either effective value
is nonzero, expansion duplicates the left binding, pitch and wave/loop windows
into the right lane. When a right member is bound, bit2 is set and both effective
values are zero, collapse clears the right binding and bit2 but retains the
right pitch and window values. A true stereo pair without bit2 is not replaced.
Enabled Sample Bank overrides determine the effective values independently.
Width at `0x6c` is separate and does not trigger these transitions.

The registered block contains neither actual member bindings nor the Bank
context. Identical stored templates can therefore require different topology
updates. Retained right-lane values or bit2 alone cannot determine which update
is needed, and applying a template is not an unconditional topology repair.
Detune/Dephase changes require a defined member/application context; they are
not independent scalar edits of the isolated block.

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

Common fields and effects follow their generation's parameter encoding.
A3000's four Program controllers occupy `0x31c..0x32b`, with four bytes per
record: device `0..125`, function `0..63`, type `0..3`, and signed range
`-63..63`. There is no second registered controller copy in SYSTEM. SYSTEM2
has both the legacy and current controller copies shown above.

On a current controller record change, copy its four bytes to the corresponding
legacy record and replace a legacy function above 63 with zero. A no-op edit
does not require normalizing previously unequal copies. A3000 has no such
projection. Its common flag byte `0x334` uses only bit6 for LFO synchronization;
preserve bit7. Its LFO waveform domain is `0..5`, versus current `0..6`.

A3000 has shared A/D routing, not an independent right route. Its common-block
offsets `0x07..0x0a` store alternating destinations and levels, with destination
domains `0..4` and `0..5`. SYSTEM2's current left outputs at `0x601..0x604`
project into common legacy groups at `0x633..0x636` when those outputs change.
Output2 destinations `1..5` map unchanged to legacy group2, and `6..9` map to
group1 after subtracting5. Output1 is applied afterward: `1..4` map unchanged
to group1, and `5..9` map to group2 after subtracting4. Output1 wins when both
outputs map to the same group. Other destinations produce no legacy route.
Right outputs have no additional legacy group. Clear unmatched legacy
destinations but preserve their levels.

Changing a registered effect type resets all 16 parameter words to that type's
generation-specific initial values before applying individual word edits.
Bypass and unchanged-type edits preserve the words. A3000 stores the type in
effect byte7 and retains byte6. SYSTEM2 uses byte6; changing type in slots1..3
also sets legacy byte7 to the type for `0..54`, or zero for `55..96`.
Slots4..6 have no corresponding legacy-type update. Connections and disabled
routes retain their dormant parameters.

SYSTEM2 retains six effects and port-B state regardless of whether the saving
device exposes every setting. Hardware model and storage revision are separate:
A4000 has no independently editable port-B state or effects4..6. The registered
prefixes and suffixes remain separate from editable parameters. Source-dependent
update rules are not fully specified for every registered Sample field.

## Unspecified State And Modification Limits

Header gaps, auxiliary state, native panel tails, parts of registered templates
and the unused-by-load suffixes have incomplete semantics. A stored value outside
a listed domain is not a default, and an unnamed byte is not free space.
Preserve unrelated bytes and do not use saved runtime pointers as persistent
identities. Independently constructing a complete System File requires more
initialization rules than this specification currently supplies.

For axklib's supported operations, see [Writer And Alteration](write.md).
