# Sequence Data And MIDI Conversion

axklib decodes the admitted current Yamaha A-series Sequence (`SEQU`) profile,
moves Sequence objects in portable packages, and converts the decoded timeline
to and from Standard MIDI File format 0. Package transfer and MIDI conversion
have different preservation contracts:

- `.axkseq` and `.axkvol` retain the complete Yamaha object payload byte for
  byte.
- MIDI export and import preserve the admitted musical events and their
  normalized timing. They do not promise byte-identical Yamaha event packing.

## Current SEQU Profile

A current Sequence is a complete `FSFSDEV3SPLXSEQU` object. The fields below
are part of the admitted decoder:

| Offset | Size | Encoding | Meaning |
| --- | --- | --- | --- |
| `0x32` | 16 | padded ASCII | sampler-visible Sequence object name |
| `0x54` | 16 | padded ASCII | retained internal track/lane label |
| `0x6c` | 2 | u16be | tempo in BPM when it is in the admitted 30-300 range |
| `0x7c` | 2 | u16be | current timeline format version; must be `1` |
| `0x7e` | 2 | u16be | ticks per quarter note; current media uses `96` |
| `0x80` | 4 | u32be | first absolute event tick |
| `0x84` | variable | timeline blocks | events through the terminal end-of-track block |

The public object name and the internal track/lane label are distinct fields.
Real sampler-authored objects can use different values. Renaming a Sequence
therefore changes only the object name at `0x32`; package transfer preserves
both fields unchanged. The exact user-facing role of every byte surrounding the
track/lane label is not yet public writer input.

Each timeline block starts with:

| Field | Size | Encoding |
| --- | --- | --- |
| next block tick | 4 | u32be |
| event count | 2 | u16be |
| events | variable | native message bytes followed by `0xfd` |
| padding | 0-3 | zero bytes to a four-byte boundary |

The tick for the first block comes from `0x80`. A nonterminal block supplies
the next block's absolute tick in its header. The terminal block contains an
end-of-track event and has a zero next tick.

The decoder expands native running status and exposes:

- MIDI channel voice events;
- meta events;
- system-exclusive events as decoded metadata.

Malformed block sizes, missing terminators, backward ticks, events following
end-of-track, and unsupported system events fail closed. System-exclusive
events are retained by the decoder but are not admitted for MIDI conversion.

## Standard MIDI File Export

`sequence_to_smf0()` writes one `MThd` and one `MTrk` chunk:

- format `0`;
- one track;
- the Sequence PPQN value;
- delta times derived from decoded absolute ticks;
- explicit channel status bytes;
- standard length-prefixed meta events;
- one terminal end-of-track event.

Native pitch bend stores the admitted seven-bit value. MIDI export represents
it as a 14-bit pitch-bend message with a zero low byte.

## Standard MIDI File Import

`smf0_to_current_sequence()` accepts a bounded format-0 file with one PPQN
track. It normalizes absolute ticks to the current 96 PPQN timeline and creates
one current `SEQU` object.

Import admits channel voice messages and meta events that can be represented
without ambiguity. It rejects:

- MIDI format 1 or 2;
- SMPTE timing;
- SysEx and system-common messages;
- pitch bend with a nonzero low seven-bit value;
- meta payloads containing the native `0xfd` event terminator;
- missing or nonterminal end-of-track;
- names outside 1-16 printable ASCII bytes;
- size, event-count, tick, and variable-length quantity overflows.

The imported object name and initial track/lane label use the requested
Sequence name. Subsequent Sequence rename changes only the object name.

Tick normalization rounds to the nearest current tick. Multiple input events
that normalize to the same tick retain their source order. Re-exporting an
imported Sequence can choose a different valid running-status packing, so
validation compares decoded event kind, message, order, and tick rather than
raw timeline bytes.

## Portable Packages

A standalone Sequence package uses:

- manifest kind `sequence`;
- filename extension `.axkseq`;
- one or more selected `SEQU` roots;
- no relationship edges or relocation descriptors.

Sequences have no admitted external object dependency. Exact package import
copies the raw payload, while an optional rename changes only the current
object-name field. A complete `.axkvol` may contain Sequence objects alongside
Programs and sample data without blocking portability.

## Compatibility Boundary

The block grammar, current version and PPQN, decoded event families, and
absence of object-reference relocation are backed by independent real-media
objects and current sampler behavior. Byte-preserving transfer has also been
played successfully on an A4000. MIDI-authored object creation and
rename/save-back remain subject to the current hardware writer profile until
the adjacent hardware roundtrip is complete.

Unknown header bytes remain preserved data. They are not exposed as
speculative writer parameters.
