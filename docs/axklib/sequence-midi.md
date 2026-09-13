# Sequence Transfer And MIDI Conversion

This page describes axklib operations. The binary layout is specified in
[Sequence Data](sequences.md). Byte-preserving packages and MIDI conversion have
different contracts: packages retain object bytes, while MIDI conversion retains
the supported musical events and normalized timing, not native byte packing.

## Decoded Tempo Metadata

`headerTempoBpm` is the optional rounded Yamaha header value. `tempoEvents`
retains each admitted Set Tempo as an absolute tick and exact integer
microseconds per quarter note. `effectiveInitialTempoMicrosecondsPerQuarterNote`
is the last valid Set Tempo at tick zero, otherwise the valid header converted
to microseconds, otherwise `500000` (120 BPM). Later tempo changes never replace
that initial value. These are metadata fields, not additional stored header words.

## Standard MIDI File Export

MIDI export writes one `MThd` and one `MTrk` chunk:

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

MIDI import accepts a bounded format-0 file with one PPQN
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

Import inspection reports event and byte counts, Controller Change numbers, and
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

A header-valid `SEQU` object whose timeline cannot be decoded may also be moved
as an opaque Sequence. The package records format `unknown`, has no relationships
or relocation descriptors, and emits `SEQUENCE_PAYLOAD_PRESERVED_OPAQUE` as a
nonfatal issue. Import requires an explicit choice for each such Sequence:
**Preserve unchanged** copies its original event bytes, while **Skip Sequence**
omits only that standalone node. There is no automatic repair and no default
choice. An explicit rename may change only the 16-byte object-name field at
`0x32`. This recovery contract does not verify MIDI conversion, editing, or
sampler playback. Other undecodable object types remain outside the
portable-package profile.

An opaque Sequence already present in an SFS target is not an import dependency
by itself. An unrelated package import may proceed, but the plan reports that
target Sequence and records its exact placement and payload hash. Post-write
verification must find the same bytes at the same placement; otherwise the
whole mutation fails and rolls back.

## Compatibility Boundary

The current block grammar, PPQN, decoded channel/meta/SysEx families, long delta
times, and tempo-map behavior are compatible with import and save-back on an
A4000 running system software 1.50. Byte-preserving Sequence transfer is also
playback-compatible. Direct playback and rename/save-back of a newly authored
Sequence remain outside the verified writer profile.

The Yamaha A5000/A4000 manual admits format-0 `.MID` import from DOS-format
floppy or hard-disk media and ISO9660 CD-ROM. This is a sampler interoperability
statement, not an axklib claim that every DOS hard-disk container is currently
writable.

Unknown header bytes remain preserved data. They are not exposed as
speculative writer parameters.
