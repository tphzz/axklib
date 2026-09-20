---
title: A-Series Sequence Data (SEQU)
---

# A-Series Sequence Data (SEQU)

A current A-series Sequence is a complete `FSFSDEV3SPLXSEQU` object. This page
describes its stored timeline. Package transfer and MIDI conversion are covered
in [A-Series Sequence Transfer And MIDI Conversion](sequence-midi.md).

## Current SEQU Profile

A current Sequence is a complete `FSFSDEV3SPLXSEQU` object. The fields below
describe the current timeline layout:

| Offset | Size | Encoding | Meaning |
| --- | --- | --- | --- |
| `0x32` | 16 | padded ASCII | sampler-visible Sequence object name |
| `0x54` | 16 | padded ASCII | retained internal track/lane label |
| `0x6c` | 2 | u16be | rounded sampler header tempo in BPM when it is in the admitted 30-300 range |
| `0x7c` | 2 | u16be | current timeline format version; must be `1` |
| `0x7e` | 2 | u16be | ticks per quarter note; current media uses `96` |
| `0x80` | 4 | u32be | first absolute event tick |
| `0x84` | variable | timeline blocks | events through the terminal end-of-track block |

The object name and track/lane label are distinct and may differ. Renaming the
object does not require changing the lane label. Surrounding unnamed header
bytes have unspecified meaning and must be preserved.

Each timeline block starts with:

| Field | Size | Encoding |
| --- | --- | --- |
| next block tick | 4 | u32be |
| event count | 2 | u16be |
| events | variable | native message bytes followed by `0xfd` |
| padding | 0-3 | zero bytes to a four-byte boundary |

The tick for the first block comes from `0x80`. A nonterminal block supplies
the next block's absolute tick in its header. The terminal block contains an
end-of-track event. The stored next-tick field of that terminal block is not
used and can be nonzero. Zero is the canonical unused value. End-of-track,
not this unused value, terminates the timeline.

Events use native running status and include MIDI channel voice messages,
meta events and system-exclusive messages. Native pitch bend stores a seven-bit
value, not an arbitrary 14-bit MIDI bend. Notes use separate Note On and Note
Off events rather than a duration field.

Tempo exists both as rounded integer BPM in the header and as precise MIDI
Set Tempo (`FF 51`) events. A Set Tempo contains three data bytes encoding
nonzero integer microseconds per quarter note. The current tempo domain is
30..300 BPM (`200000..2000000` microseconds). Tick-zero events are distinct
from later tempo changes; a later event must not replace the header's initial
tempo meaning. Host fallback conventions are described in the conversion page.

Block sizes must fit the payload, event terminators must be present and absolute
ticks must not move backward. End-of-track is terminal. Unknown header fields
have no independent modification rules; retain them instead of guessing defaults.
