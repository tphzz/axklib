# Sequence Data And MIDI Conversion

axklib decodes the admitted current Yamaha A-series Sequence (`SEQU`) profile,
moves Sequence objects in portable packages, and converts the decoded timeline
to and from Standard MIDI File format 0. Package transfer and MIDI conversion
have different preservation contracts:

- `.axkseq` and `.axkvol` retain the complete Yamaha object payload byte for
  byte.
- MIDI export and import preserve the admitted musical events, tempo map, time
  signatures, and normalized timing. They do not promise byte-identical Yamaha
  event packing.

## Current SEQU Profile

A current Sequence is a complete `FSFSDEV3SPLXSEQU` object. The fields below
are part of the admitted decoder:

| Offset | Size | Encoding | Meaning |
| --- | --- | --- | --- |
| `0x32` | 16 | padded ASCII | sampler-visible Sequence object name |
| `0x54` | 16 | padded ASCII | retained internal track/lane label |
| `0x6c` | 2 | u16be | rounded sampler header tempo in BPM when it is in the admitted 30-300 range |
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

Tempo is represented at two distinct levels. `headerTempoBpm` is the optional
rounded Yamaha header value. `tempoEvents` retains every admitted MIDI Set
Tempo (`FF 51`) event as an absolute tick plus the exact integer microseconds
per quarter note. `effectiveInitialTempoMicrosecondsPerQuarterNote` is the last
valid Set Tempo event at tick zero, otherwise the valid header tempo converted
to microseconds, otherwise the Standard MIDI default of `500000` (120 BPM).
Later tempo events never replace the initial-tempo value.

Malformed block sizes, missing terminators, backward ticks, events following
end-of-track, and unsupported system events fail closed. System-exclusive
events are retained by the decoder. MIDI conversion admits only the bounded
System Exclusive profile described below. A Set Tempo event must contain
exactly three data bytes, encode a nonzero value, and remain in the admitted
30-300 BPM range (`200000..2000000` microseconds per quarter note).

## Standard MIDI File Export

`sequence_to_smf0()` writes one `MThd` and one `MTrk` chunk:

- format `0`;
- one track;
- the Sequence PPQN value;
- delta times derived from decoded absolute ticks;
- explicit channel status bytes;
- standard length-prefixed admitted `F0` and escaped `F7` events;
- standard length-prefixed meta events;
- one terminal end-of-track event.

Every admitted Set Tempo and time-signature event is written in source order at
its decoded tick. If the timeline has no tick-zero Set Tempo event, export
injects exactly one at tick zero using the valid Yamaha header tempo or the 120
BPM default. It does not round an existing precise tempo through the integer
header field. If several tempo events occur at tick zero, their order is
preserved and the last one defines the effective initial tempo.

Native pitch bend stores the admitted seven-bit value. MIDI export represents
it as a 14-bit pitch-bend message with a zero low byte.

## Standard MIDI File Import

`smf0_to_current_sequence()` accepts a bounded format-0 file with one PPQN
track. It normalizes absolute ticks to the current 96 PPQN timeline and creates
one current `SEQU` object.

Import admits channel voice messages and meta events that can be represented
without ambiguity. System Exclusive handling is an explicit per-import policy:

- `reject` fails before alteration when the file contains any `F0` or `F7`
  event;
- `exclude` removes all System Exclusive events while retaining the absolute
  ticks and source order of every admitted event;
- `preserve` retains every event when all `F0` events end in `F7`, all bytes
  between those delimiters are seven-bit data, and every escaped `F7` event has
  at least one seven-bit data byte. A file containing any other form fails
  before alteration.

`inspect_smf0()` reports event and byte counts, Controller Change numbers, and
manufacturer IDs without returning opaque System Exclusive payload bytes.
Axkdeck performs this inspection before import. Its Include SysEx events option
is available only when every System Exclusive event in every selected file
matches the admitted preservation profile. The option remains unchecked by
default; leaving it unchecked imports the Sequence with those events explicitly
excluded. Unsupported files cannot enable it.

Outside that explicit policy, import rejects:

- MIDI format 1 or 2;
- SMPTE timing;
- system-common messages;
- pitch bend with a nonzero low seven-bit value;
- malformed, zero, or out-of-profile Set Tempo values;
- meta payloads containing the native `0xfd` event terminator;
- missing or nonterminal end-of-track;
- names outside 1-16 printable ASCII bytes;
- size, event-count, tick, and variable-length quantity overflows.

The imported object name and initial track/lane label use the requested
Sequence name. Subsequent Sequence rename changes only the object name. Import
retains all Set Tempo and time-signature events. The Yamaha header tempo is the
rounded last tick-zero Set Tempo value, or 120 BPM when the file has no
tick-zero tempo; a later tempo change is never promoted into the header.

Tick normalization rounds to the nearest current tick. Multiple input events
that normalize to the same tick, including tempo changes, retain their source
order. Re-exporting an imported Sequence can choose a different valid
running-status packing, so validation compares decoded event kind, message,
order, and tick rather than raw timeline bytes.

Notes use ordinary channel Note On and Note Off events; axklib does not impose a
separate duration encoding. Long notes therefore retain their end tick while
each exported SMF event delta remains within the four-byte, 28-bit MIDI VLQ
limit. A larger gap fails explicitly rather than truncating or overflowing.

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
played successfully on an A4000. An A4000 running system software 1.50 imported
and saved back four format-0 MIDI cases covering a precise tick-zero tempo, a
later tempo change, and the no-Set-Tempo default. It also imported and saved
back five event-profile cases covering every admitted channel family, CC 71/74
sweeps, a 65536-tick note, descriptive and timing meta events, one complete
`F0 ... F7` event, and one escaped `F7` event. Every decoded event, byte, order,
and tick in those nine returned timelines matches axklib's canonical import.
Direct playback and rename/save-back of an axklib-authored object remain
unavailable until their validation roundtrip is complete.

The Yamaha A5000/A4000 manual admits format-0 `.MID` import from DOS-format
floppy or hard-disk media and ISO9660 CD-ROM. This is a sampler interoperability
statement, not an axklib claim that every DOS hard-disk container is currently
writable. Tempo-map import and save-back behavior are covered by both automated
codec tests and the multi-case ISO hardware roundtrip. The returned media did
not independently record the sampler's visible tempo display during playback.

Unknown header bytes remain preserved data. They are not exposed as
speculative writer parameters.
