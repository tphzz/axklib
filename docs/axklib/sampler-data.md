# Sampler Data Structures

Yamaha A-series object files share a header across SFS hard disks, FAT12
floppies, ISO9660 CD-ROMs and A3K volume archives. The container locates each
file; the object payload defines its type and contents.

This page describes SMPL (Wave Data), SBNK (Sample), SBAC (Sample Bank) and
PROG (Program). See [System Files](system-files.md) for SYSTEM/SYSTEM2 and
[Sequence Data](sequences.md) for SEQU. Offsets are hexadecimal and relative
to the object start unless a table specifies another base. Multi-byte numeric
fields are big-endian unless stated otherwise. Descriptive field names below
are labels for the byte layout, not an API or report schema.

## Shared Object Header

Supported current object payloads begin with:

```text
FSFSDEV3SPLX<type>
```

Header layout:

| Offset | Size | Type | Field | Meaning |
| --- | ---: | --- | --- | --- |
| `0x00` | 12 | ASCII | magic | `FSFSDEV3SPLX`. |
| `0x0c` | 4 | ASCII | type | Object type tag. |
| `0x10` | 4 | u32be | header_size | Object header size. For `SMPL` exact export, this is the stored waveform byte start. |
| `0x14` | 4 | u32be | unknown | Preserved layout selector. The object loader uses it to choose the object-body length field, but the complete value domain is not yet named. |
| `0x18` | 4 | u32be | record_size_or_header_used | Object-body read length when `0x14 < 4`; retained as an object-specific raw field. |
| `0x1c` | 4 | u32be | payload_bytes | Object payload byte-count field. For `SMPL`, this is the complete logical Wave Data byte count. |
| `0x20` | 4 | u32be | payload_bytes | For `SMPL`, the Wave Data bytes physically stored in this file segment. |
| `0x24` | 4 | u32be | payload_offset | For `SMPL`, this file segment's byte offset in the complete logical Wave Data. |
| `0x28` | 2 | u16be | sample_rate | `SMPL` sample-rate field. Empty for other types. |
| `0x2a` | 2 | u16be | bytes_per_sample | `SMPL` stored sample width. Empty for other types. |
| `0x32` | 16 | ASCII | name | Object header name, trimmed of trailing NUL and spaces. |

The fields through the name occupy at least `0x42` bytes. Names may end in
NUL bytes or ASCII spaces; invalid text bytes do not define another object type.

## Object Type Tags

| Type | Role |
| --- | --- |
| `SMPL` | Wave Data storage object. Holds mono PCM and playback-window metadata. |
| `SBNK` | Sampler-visible Sample object. Stores Sample parameters and links to Wave Data storage. |
| `SBAC` | Sampler-visible Sample Bank object. Contains member Sample names. |
| `PROG` | Program object, Program display name, effects, controller data, and assignment rows. |
| `SEQU` | Sequence object. Current timeline events, timing, tempo, and MIDI conversion are decoded. |
| `PRF3` | Profile/preference-style object. Includes partition-level SYSTEM/SYSTEM2; other inner layouts are unspecified here. |
| other known tags | Other type-specific layouts are outside this specification. |

## Container Filename Versus Payload Identity

Every object file is complete from byte `0x00`; container allocation does not
replace or remove the shared object header. The container filename locates the
file, while the four-byte tag at payload offset `0x0c` determines its type and
the embedded header supplies its sampler object name.

| Container | Placement filename | Type and display-name source |
| --- | --- | --- |
| FAT12 floppy | DOS 8.3 name such as `AUTHORED.001` | `FSFSDEV3SPLX<type>` and embedded object name. A numeric extension is not a type code. |
| CD-ROM ISO | `<group>/Fnnn/<type>/Fnnn`; category `0000` maps display names to files | Embedded type and name remain authoritative. The category name and catalog are independently checked placement metadata. |
| Standalone file | Host filename chosen by the caller | Entire file begins with the shared magic and type tag. |
| A3K archive | Terminal-index path such as `Volume \SMPL\Object` | Embedded type and name are authoritative. The index path is redundant placement metadata. |
| SFS image | SFS directory record and object ID rather than a host filename | Embedded type and name, with SFS placement retained separately. |

`SMPL` and `SBNK` have variable total file sizes. Consumers must use the
big-endian length and offset fields in the object rather than inferring payload
boundaries from a FAT cluster count, ISO extent padding, or an `Fnnn` name.
`SEQU` contains a timeline; see [Sequence Data](sequences.md).
`PRF3` includes SYSTEM/SYSTEM2 files, described in [System Files](system-files.md).
Other PRF3 inner formats are unspecified.

## SMPL: Wave Data Object

`SMPL` objects store mono Wave Data. Stereo pairs are represented by SBNK
member relationships rather than interleaved SMPL payloads.

The PCM layout described here requires transfer-control byte `0x84` to be
exactly `0x30`. Extra-bit values such as `0x31` and `0xb0` do not establish
the same PCM encoding. Other complete control-byte values have unspecified
audio interpretation and must not be converted as this PCM profile.
This byte is independent of the object-layout selector at `0x14`.

Current compact metadata fields:

| Offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `0x28` | 2 | u16be | sample_rate |
| `0x2a` | 2 | u16be | bytes_per_sample |
| `0x30` | `0x7c` | bytes | compact record |
| `0x54` | 16 | ASCII | embedded_container_name |
| `0x74` | 4 | u32be | transient_name_hash_next_handle |
| `0x78` | 4 | u32be | wave_data_reference_value |
| `0x7c` | 2 | u16be | sample_rate_duplicate |
| `0x7e` | 1 | u8 | root_key_midi_note |
| `0x7f` | 1 | s8 | fine_tune_cents |
| `0x84` | 1 | u8 | pcm_transfer_control |
| `0x85` | 1 | u8 | loop_mode |
| `0x8e` | 4 | u32be | wave_start_frame |
| `0x92` | 4 | u32be | wave_length_frames |
| `0x96` | 4 | u32be | loop_start_frame |
| `0x9a` | 4 | u32be | loop_length_frames |
| `0xaa` | 2 | u16be | transient_512_byte_block_counter |

Derived loop values:

```text
loop_end_frame_inclusive = loop_start + loop_length - 1
loop_end_frame_exclusive = loop_start + loop_length
loop_end_frame_a4000_ui  = loop_start + loop_length
```

### Compact-record handling

The compact record occupies `0x7c` bytes beginning at object offset `0x30`.
Its fields use the following overlapping copy layout:

| Object range | Runtime range | Length |
| --- | --- | ---: |
| `0x30..0x42` | `+0x00..+0x12` | `0x13` |
| `0x4a..0x6e` | `+0x1e..+0x42` | `0x25` |
| `0x74..0x77` | `+0x3c..+0x3f` | `0x04` |
| `0x78..0x8d` | `+0x40..+0x55` | `0x16` |
| `0x8e..0xab` | `+0x58..+0x75` | `0x1e` |

The later copies replace overlapping runtime bytes initially populated from
`0x68..0x6e`. This produces two aliases on disk:
`0x68..0x6b == 0x74..0x77` and
`0x6c..0x6e == 0x78..0x7a`. The latter is only a three-byte prefix; byte
`0x6f` is not part of the reference.

This table describes the A4000/A5000 common-copy path. The A3000 path
copies only `0x30..0x41` in the first range; it does not transfer byte `0x42`.
Preserve that byte as opaque on read. A Program's layout selector alone does
not identify which model last saved its shared common record.

Object ranges `0x43..0x49` and `0x6f..0x73` are non-semantic residue, not
object fields. Canonical new records use zero there. In particular, `0x6c..0x6f`
must not be interpreted as a four-byte group identifier.

The normalized object values at `0x7c`, `0x7e`, `0x7f`, `0x80`, `0x84`,
`0x85`, `0x8e`, `0x92`, `0x96`, and `0x9a` supply playback metadata. The
loader derives the Wave Data end and loop end by adding each start and length
pair. The embedded source/container text at `0x54` is preserved by load and
save normalization. Current SFS objects store the containing Volume name; it is
source-dependent outside SFS and is not a second Wave Data identity. Reserved
ranges `0x82..0x83`, `0x86..0x8d`, and `0x9e..0xa9` have no supported playback
meaning; canonical new records use zero there.

The value at `0x74..0x77` is the transient name-hash collision-chain next
handle. `0xaa..0xab` is a transient 512-byte transfer counter whose complete
continuation lifecycle remains unknown. Neither is a user-facing Wave Data
property. `0x78..0x7b` is a rebuilt runtime storage/cache reference used during
Sample relationship resolution; it is not a persistent globally unique object
identifier. Canonical authoring zero-initializes the residue ranges and the
new-object hash-chain handle and transfer counter instead of reproducing
captured runtime state. `0x84 & 0x30` selects one of four internal PCM transfer
codes; canonical authoring emits `0x30`. Other complete control-byte values have unspecified audio interpretation,
regardless of the masked selector. Individual common-state bits and the complete
source/import descriptor schema have no defined independent modification rules.
Preserving their raw values does not establish that they can safely be edited.

The shared object loader compares the raw field at `0x14` with `4`: values below
`4` select the body length at `0x18`, while later layouts select `0x1c`; value
`1` also invokes a post-load conversion callback. The complete selector domain is unspecified; retain both fields unchanged
when their layout is not being converted.

Loop-mode display values:

| Raw | Label |
| ---: | --- |
| `0` | `-->` |
| `1` | `->0` |
| `2` | `->0->` |
| `3` | `<--` |
| `4` | `One->` |
| `5` | `One<-` |

There is no separate SMPL coarse-tune field; coarse tuning belongs to SBNK.
Wave and loop positions use frame coordinates. A repeating loop must have a
nonzero length and fit inside its playback window; an exclusive endpoint can
equal the window end. One-shot modes play through independently of key release.

PCM byte representation:

These mappings apply only to transfer control `0x30`:

| Stored width | Stored bytes | WAV bytes |
| ---: | --- | --- |
| `2` | 16-bit stored samples in big-endian byte order | Byte-swapped to little-endian 16-bit WAV PCM. |
| `1` | 8-bit PCM | Copied directly to WAV frames. |

`header_size` is the start offset of waveform bytes inside a `SMPL` file.
Complete objects use `payload_offset == 0` and
`payload_bytes == payload_bytes`. Yamaha multi-floppy saves can split
one logical Wave Data object across several disk files. Those files repeat the
same header, set `payload_bytes` to the local segment size, and set
`payload_offset` to the segment's byte offset. A complete logical waveform requires contiguous segments covering the byte
count at `0x1c`. Segment identity comes from the repeated object metadata,
not solely from host filenames, which may differ between disks. Missing or
overlapping segments do not define complete PCM.

Generated images may store a short compatibility tail after the logical waveform
frames. In that case the complete logical byte count includes the tail, while
`wave_length_frames` and `loop_length_frames` describe the logical
sample window.

The declared stored width determines PCM interpretation; a different numeric
sample representation must not be inferred from payload appearance.

## SBNK: Sample Object

`SBNK` objects are sampler-visible Samples. They link to Wave Data storage and
carry most Sample parameters.

### On-Disk Common Record

Current `SBNK` and `SBAC` objects use the same normalized common-record
mapping as current Wave Data. The stored fields are:

| Offset | Size | Classification |
| --- | ---: | --- |
| `0x030` | 1 | object class (`0x10` for SBNK, `0x11` for SBAC) |
| `0x031` | 1 | packed lifecycle/dirty state; preserve the raw byte |
| `0x032..0x041` | 16 | object name |
| `0x042` | 1 | opaque-preserved common state transferred by the load/save transforms |
| `0x043..0x049` | 7 | untransferred saver residue; canonical new records use zero |
| `0x04a..0x053` | 10 | opaque-preserved common state |
| `0x054..0x063` | 16 | source-dependent embedded container text |
| `0x064..0x067` | 4 | opaque-preserved common state |
| `0x068..0x06b` | 4 | alias of the transient handle at `0x074..0x077` |
| `0x06c..0x06e` | 3 | alias of the body prefix at `0x078..0x07a` |
| `0x06f..0x073` | 5 | untransferred saver residue; canonical new records use zero |
| `0x074..0x077` | 4 | transient name-hash collision-chain handle |

The embedded text at `0x054` is not a 24-byte Sample instrument-name field:
the surrounding bytes belong to distinct common-state and alias lanes. Exact
alterations preserve opaque state, including `0x042`. The load/save transforms
retain that byte as durable object state. Its semantic role is unspecified. New objects use zero residue and transient handles, with both aliases rebuilt.

### Member Resolution Fields

| Offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `0x078` | 16 | ASCII | left member Wave Data name |
| `0x088` | 16 | ASCII | right member Wave Data name |
| `0x098` | 4 | u32be | left transient runtime object-pointer slot |
| `0x09c` | 4 | u32be | right transient runtime object-pointer slot |
| `0x0a0` | 4 | u32be | left cached Wave Data reference value |
| `0x0a4` | 4 | u32be | right cached Wave Data reference value |
| `0x0c0` | 4 | u32be | linked Programs 001-032 bitmap |
| `0x0c4` | 4 | u32be | linked Programs 033-064 bitmap |
| `0x0c8` | 4 | u32be | linked Programs 065-096 bitmap |
| `0x0cc` | 4 | u32be | linked Programs 097-128 bitmap |

The A4000 resolves each active member by its 16-byte Wave Data name, populates
`0x098/0x09c` with runtime pointers and caches the resolved SMPL reference value
at `0x0a0/0x0a4`. A persisted cached value can be stale. Neither cached value
nor pointer is an authoritative disk identity.

A unique local name identifies a member. Duplicate local names are ambiguous;
a matching cached value alone does not resolve an otherwise missing member.

The ordinary stereo layout uses the left and right member fields on one
`SBNK`. Some source media instead stores stereo as two sibling `SBNK` objects
under the same `SBAC` Sample Bank, with sampler-facing names ending in `-L` and `-R`.
Each sibling uses its own left-member Wave Data name. The two SBNK records
remain distinct Samples even when they form a stereo pair.

Program-link bitmap decoding:
```text
for word_index in 0..3:
    base_program = word_index * 32 + 1
    for bit_index in 0..31:
        if word & (1 << bit_index):
            linked_program = base_program + bit_index
```

### Sample Parameter Window

The extended sample parameter window starts at `0x0a8`, contains 224 bytes, and
ends at `0x187`. Two current logical object extents occur in real media:

- a `0x164`-byte object ends after parameter offset `0xbb`; and
- a `0x188`-byte object includes the complete 224-byte parameter window.

For `SBNK`, calculate this extent as `0x30 + payload_bytes`.
Sampler-authored Samples commonly store zero in the generic `header_size` field,
so `header_size + payload_bytes` is not an SBNK size formula.

The first 24 bytes at `0x0a8..0x0bf` contain six four-byte controller records.
Extended objects repeat these at `0x164..0x17b`. On A4000 the extended copy takes
precedence when it is present; shorter objects use the prefix copy. Consistent
controller updates must maintain both copies. Unrelated edits preserve a
preexisting mismatch rather than choosing new values for it.

Controller record domains are Device `0..126`, Function `0..36`, Type `0..3`,
and signed Range `-63..+63`. Device values `121..126` are special selectors,
not ordinary MIDI controller numbers.

The following tables give physical parameter offsets. The same parameter
layout is split across the SBAC prefix and terminal block as described below.

| Offset | Type | Field |
| --- | --- | --- |
| `0x0d0` | u8 | sample_flags |
| `0x0d1` | u8 | mapout_flags |
| `0x0d2` | u8 | midi_receive_channel |
| `0x0d3` | u8 | pitch_bend_type |
| `0x0d4` | u8 | pitch_bend_range |
| `0x0d5` | u8 | coarse_tune |
| `0x0d6` | u8 | left_root_key |
| `0x0d7` | u8 | right_root_key |
| `0x0d8` | u16be | left_sample_rate |
| `0x0da` | u16be | right_sample_rate |
| `0x0dc` | s8 | left_fine_tune_cents |
| `0x0dd` | s8 | right_fine_tune_cents |
| `0x0de` | u16be | pitch_base_word |
| `0x0e0` | u16be | secondary_pitch_base_word |
| `0x0e2` | u8 | key_range_high |
| `0x0e3` | u8 | key_range_low |
| `0x0e5` | u8 | loop mode |
| `0x0e6` | u16be | loop_tempo |

Key-range values `0..127` are concrete MIDI limits. The `Orig` sentinel is
raw `128` for the high limit at `0x0e2` and raw `255` for the low limit at
`0x0e3`. It resolves to the member root key; compare effective endpoints after
that substitution. An empty right-member name in a single-member Sample means
the right lane is inactive, not a second playback region.

| Offset | Type | Field |
| --- | --- | --- |
| `0x0e8` | u32be | left_wave_start_address |
| `0x0ea` | u16be | left_wave_start_low16 |
| `0x0ec` | u32be | right_wave_start_address |
| `0x0ee` | u16be | right_wave_start_low16 |
| `0x0f0` | u32be | left_wave_length_frames |
| `0x0f4` | u32be | right_wave_length_frames |
| `0x0f8` | u32be | left_loop_start_frame |
| `0x0fc` | u32be | right_loop_start_frame |
| `0x100` | u32be | left_loop_length_frames |
| `0x104` | u32be | right_loop_length_frames |
| `0x108` | u8 | start_address_velocity_sensitivity |
| `0x109` | u8 | filter_type |
| `0x10a` | u8 | filter_cutoff |
| `0x10b` | u8 | filter_q_width |
| `0x10c` | u8 | filter_cutoff_key_scaling_break1 |
| `0x10d` | u8 | filter_cutoff_key_scaling_break2 |
| `0x10e` | u8 | filter_cutoff_key_scaling_level1 |
| `0x10f` | u8 | filter_cutoff_key_scaling_level2 |
| `0x110` | u8 | filter_cutoff_velocity_sensitivity |
| `0x111` | u8 | filter_q_width_velocity_sensitivity |
| `0x112` | u8 | expand_detune |
| `0x113` | u8 | expand_dephase |
| `0x114` | u8 | expand_width |
| `0x115` | u8 | random_pitch |
| `0x116` | u8 | sample_level |

The member start, length, and loop fields are frame addresses in the linked
physical Wave Data object. They define the Sample's playable window and can
select only one segment of a larger shared `SMPL` payload. Loop Divide media is
a common example: several Samples have different start frames while referencing
the same Wave Data. The full left/right wave-start words are decoded for this
read path. Real images use nonzero upper halves, including shared stereo starts,
so the words must not be reduced to their overlapping low-16 aliases.
For Sample playback, resolve each active member to its SMPL, begin at the
member's wave start, and use that member's wave length. Loop coordinates are
absolute in the linked Wave Data; subtract the member start for window-relative
playback. The SBNK loop mode governs Sample playback, independently of the SMPL
selector. Do not clamp an invalid window to conceal an out-of-bounds reference.

Sample level is `0..127`. A4000 parameter domains include pitch-bend range
`0..24`, filter type `0..16`, cutoff/Q velocity sensitivity `-63..63` plus
`64..68` for `Rnd1..Rnd5`, and Portamento rate/time `1..127`. Member root key
is `0..127` and fine tune is `-63..63`.

`sample_flags` is read-only topology state. Bit `0` records Sample Bank
membership, bit `1` records mono topology, and bit `2` records expanded-mono
topology. New objects use `0x02` for ordinary single-source mono and `0x00`
for two-source stereo, then derive bit `2` for a single-source Sample whose
Expand Detune or Expand Dephase is nonzero. Expand Detune accepts `-7..7`, and
Expand Dephase and Width accept `-63..63`; true two-source stereo rejects
nonzero detune or dephase. Membership and expanded-mono changes affect separate bits. Duplicate-source
expanded objects require preservation of their existing topology; their
independent construction rules are unspecified.

`mapout_flags` is a packed byte, not one wholly semantic field. Bits
`7..6` are EQ Type, bit `4` is Fixed Pitch, bit `2` is Key Crossfade, and bit
`1` is Poly/Mono (`0=Poly`, `1=Mono`). Bit `0` is a derived cache: it is set
exactly when Sample Portamento Type is `Pgm` (raw `1`) and clear for the other
types. A Portamento Type change must refresh that bit while preserving other lanes. Bit `3` is a legacy-layout default selector:
when an older object lacks the extended parameter
tail, the bit initializes Velocity X-Fade High and Low to `5` rather than `0`.
Current objects store both values directly, so bit `3` remains diagnostic and
preserve-only. Bit `5` has
unspecified meaning and must be preserved. Fresh objects write zero for bits `5` and `3`;
template-based edits retain them.

`0x152..0x15b` store five signed big-endian Q13 Sample EQ biquad coefficients
in `b1`, `b2`, `b0`, `-a1`, `-a2` order. They are derived from EQ Type,
Frequency, Gain and Width, not independent controls. An EQ change requires a
consistent complete vector; an unrelated edit must preserve it.
Bytes `0x13f..0x140` and `0x142` travel with AEG Sustain at `0x141` but have
no separate parameter meaning specified here. Bytes `0x14d..0x150` contain
internal synthesis state without independent user controls. Preserve both ranges.

| Offset | Type | Field |
| --- | --- | --- |
| `0x117` | u8 | pan |
| `0x118` | u8 | velocity_low_limit |
| `0x119` | u8 | velocity_offset |
| `0x11a` | u8 | velocity_range_high |
| `0x11b` | u8 | velocity_range_low |
| `0x11c` | u8 | level_scaling_break1 |
| `0x11d` | u8 | level_scaling_break2 |
| `0x11e` | u8 | level_scaling_level1 |
| `0x11f` | u8 | level_scaling_level2 |
| `0x120` | u8 | velocity_sensitivity |
| `0x121` | u8 | alternate_group |
| `0x122` | u8 | sample_eq_frequency |
| `0x123` | u8 | sample_eq_gain |
| `0x124` | u8 | sample_eq_width |
| `0x125` | u8 | filter_cutoff_distance |
| `0x126` | u8 | feg_attack_rate |
| `0x127` | u8 | feg_decay_rate |
| `0x128` | u8 | feg_release_rate |
| `0x129` | u8 | feg_init_level |
| `0x12a` | u8 | feg_attack_level |
| `0x12b` | u8 | feg_sustain_level |
| `0x12c` | u8 | feg_release_level |
| `0x12d` | u8 | feg_rate_key_scaling |
| `0x12e` | u8 | feg_rate_velocity_sensitivity |
| `0x12f` | u8 | feg_attack_level_velocity_sensitivity |
| `0x130` | u8 | feg_level_velocity_sensitivity |
| `0x131` | u8 | peg_attack_rate |
| `0x132` | u8 | peg_decay_rate |
| `0x133` | u8 | peg_release_rate |
| `0x134` | u8 | peg_init_level |
| `0x135` | u8 | peg_attack_level |
| `0x136` | u8 | peg_sustain_level |
| `0x137` | u8 | peg_release_level |
| `0x138` | u8 | peg_rate_key_scaling |
| `0x139` | u8 | peg_rate_velocity_sensitivity |
| `0x13a` | u8 | peg_level_velocity_sensitivity |
| `0x13b` | u8 | peg_range |
| `0x13c` | u8 | aeg_attack_rate |
| `0x13d` | u8 | aeg_decay_rate |
| `0x13e` | u8 | aeg_release_rate |
| `0x141` | u8 | aeg_sustain_level |
| `0x143` | u8 | aeg_attack_mode |
| `0x144` | u8 | aeg_rate_key_scaling |
| `0x145` | u8 | aeg_rate_velocity_sensitivity |
| `0x146` | u8 | lfo_wave |
| `0x147` | u8 | lfo_speed |
| `0x148` | u8 | lfo_delay_time |
| `0x149` | u8 | lfo_flags |
| `0x14a` | u8 | lfo_cutoff_mod_depth |
| `0x14b` | u8 | lfo_pitch_mod_depth |
| `0x14c` | u8 | lfo_amp_mod_depth |
| `0x151` | s8 | filter_gain |
| `0x152..0x15b` | 5 x s16be Q13 | sample_eq_biquad_coefficients (`b1`, `b2`, `b0`, `-a1`, `-a2`) |
| `0x15c` | u32be | wave_end_address (derived cache) |
| `0x160` | u32be | loop_end_address (derived cache) |
| `0x17c` | u8 | velocity_xfade_high |
| `0x17d` | u8 | velocity_xfade_low |
| `0x17e` | u8 | output1 |
| `0x17f` | u8 | output1_level |
| `0x180` | u8 | output2 |
| `0x181` | u8 | output2_level |
| `0x182` | u8 | sample_portamento_type |
| `0x183` | u8 | sample_portamento_rate |
| `0x184` | u8 | sample_portamento_time |

`SBNK+0x15c` and `SBNK+0x160` are format-maintained 32-bit derived caches.
Wave end is the serialized left wave start plus left wave length; loop end
is the serialized left loop start plus left loop length. Both additions wrap
modulo `2^32`. These caches must remain consistent with their source fields and
are not independent parameters.

Sample control records are six 4-byte records at:

```text
record_offset = 0x0a8 + 0xbc + (record_index - 1) * 4
```

Each record stores `device_u8`, `function_u8`, `type_u8`, and signed `range_s8`.

## SBAC: Sample Bank Object

`SBAC` objects are shown as `B <name>` Sample Banks in user-facing trees. They
contain member rows that point by name to Sample (`SBNK`) objects.

| Offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `0x078..0x133` | 188 | bytes | First part of the canonical 224-byte Sample Parameter block. |
| `0x090..0x09f` | 16 | 4 x u32be | Linked Programs 001-128 bitmap within that parameter block. |
| `0x134..0x13f` | 12 | 3 x u32be | Pending Sample Parameter propagation bitmaps. |
| `0x140..0x143` | 4 | bytes | Reserved; preserve for existing objects. |
| `0x144` | 1 | u8 | Stored member count. |
| `0x145..0x14b` | 7 | bytes | Reserved; preserve for existing objects. |
| `0x14c + n*0x14` | 20 each | rows | Member rows, followed by any preallocated blank-row capacity. |
| Last `0x24` bytes, layout selector `0x14 >= 4` | 36 | bytes | Final part of the canonical Sample Parameter block. |

The disk layout is not the flat Sample Bank Bulk layout used by the runtime.
The loader transform reconstructs one canonical 224-byte Sample
Parameter block from disk `0x078..0x133` followed by the terminal 36 bytes.
The serializer applies the inverse transform. For legacy objects with layout
selector `0x14 < 4`, no terminal block is stored and the loader supplies
zero/default bytes for that final 36-byte portion. Offsets `0x040..0x11f` and
`0x120..0x12b` describe the normalized runtime/Bulk representation, not the
physical SBAC object.

The member region ends at the object size for a legacy SBAC and at
`object_size - 0x24` for the current split-tail layout. Its complete-row
capacity is therefore:

```text
member_capacity = (member_region_end - 0x14c) / 0x14
```

Current-layout mutation inserts additional rows before the terminal parameter
bytes. Legacy mutation extends the row region without creating a terminal tail.
The two layouts also retain their header-length conventions: legacy objects use
`0x18 = object_size - 0x30`, while current objects use
`0x18 = object_size - 0x54` and `0x1c = object_size - 0x30`.

SBAC pending-propagation bitmap decoding:

```text
for word_index in 0..2:
    base_p2 = word_index * 32
    for bit in 0..31:
        if word & (1 << bit):
            pending_sample_parameter_p2 = base_p2 + bit
```

The `Freeze SampleBank` operation consumes these bits. For every marked P2
number, it copies the
bank's corresponding Sample Parameter value into each resolved member Sample,
clears the consumed bit, and marks the Sample Bank dirty. These words are
therefore pending operation state, not durable per-bank value-enable settings.
Only P2 `0..88` are actionable. The operation stops after `88`, and there are no
parameter-table entries for `89..95`; those seven positions are reserved bitmap
capacity; preserve them when nonzero.
Existing words are preserved by unrelated mutation; a fresh Sample Bank writes
zero.

Bank parameter values and pending propagation bits are separate: storing a
value in the bank is not equivalent to applying it to every member. Clearing
pending bits without applying their values discards the pending operation.

The four linked-Program words use the same bit numbering as the Sample bitmap:
bit zero of the first big-endian word is Program 001. Fresh image creation
derives them from Program-to-Sample-Bank assignments. Program insertion and
deletion update both the Program row and the target Sample Bank bitmap in one
transaction. Unrelated exact mutation preserves the words.

SBAC slot row layout, stride `0x14`:

| Row offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `+0x00` | 16 | ASCII | Sample (`SBNK`) member name; first byte zero means an inert row. Live names shorter than 16 bytes are ASCII-space padded. |
| `+0x10` | 4 | u32be | transient resolved-member runtime pointer residue |

The reader uses the stored member count at `0x144` to decide how many rows to
read, capped by the generation-specific member capacity. The object resolver
skips a counted row exactly when its first name byte is zero. For
every active row it resolves the stored Sample name and writes the resulting
runtime object pointer at row `+0x10`; it does not read the persisted word as
resolver input. The effective member count is therefore the number of counted
rows whose first name byte is nonzero. Blank counted rows remain inert and are
not compacted during unrelated mutation.

The stored member pointer is transient, not a persistent link identity. Zero
is a valid unresolved-pointer representation for a new row. Existing pointer
bytes need not be rewritten for unrelated changes.

Member names resolve within the bank's local volume. Multiple exact-name
candidates are ambiguous; a similarly named Sample in another volume does not
supply the missing local member.

## PROG: Program Object

`PROG` objects represent Program slots. The object header name is normally a
three-digit slot ID. The Program display name occupies `0x078..0x07f`.
Its three-byte common prefix alias is stored at `0x6c..0x6e`.

### Program Common Fields

The checked layout distinguishes legacy selectors `1`/`2` from current `4`.
Other selectors are unsupported. Legacy logical length is header `0x18` plus
`0x30`; current length is header `0x18` plus `0xe0` and must also equal header
`0x1c` plus `0x30`. Bytes beyond logical length are container padding, not
additional rows. Truncated extents and conflicting length declarations fail.

The big-endian count at `0x96` is authoritative, including zero. It must not
exceed 999 or the integral physical row capacity. Legacy records have no
terminal block. Current records end in a `0xb0`-byte parameter block at
`tail = logical_length - 0xb0`. Capacity is `(tail - 0x120) / 0x38` for current
records, or `(logical_length - 0x120) / 0x38` for legacy records.

| Offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `0x068..0x077` | 16 | bytes | raw |
| `0x080` | 1 | u8 | program_flags_ad_source_effect_connection_lfo_sync |
| `0x081` | 1 | u8 | program_lfo_cycle_wave_initial_phase |
| `0x082..0x085` | 4 | 2 x u16be | Port-A controller-reset and note-toggle channel maps. |
| `0x086` | 1 | s8 | A/D left Pan. |
| `0x087..0x08a` | 4 | bytes | Legacy A/D output destinations and levels. |
| `0x08b` | 1 | u8 | Program level. |
| `0x08c..0x08d` | 2 | bytes | Preserved common state, not writable parameters. |
| `0x08e` | 1 | s8 | Transpose. |
| `0x08f` | 1 | s8 | LFO reset channel selection. |
| `0x090` | 1 | u8 | program_portamento_type |
| `0x091` | 1 | u8 | program_portamento_rate |
| `0x092` | 1 | u8 | program_portamento_time |
| `0x093` | 1 | u8 | sample_and_hold_speed |
| `0x094` | 1 | u8 | program_lfo_tempo |
| `0x095` | 1 | s8 | LFO reset note selection. |
| `0x096..0x097` | 2 | u16be | Stored assignment count. |
| `0x110..0x11f` | 16 | 4 records | Legacy controller projection; authoritative only in legacy records. |
| `tail + 0x78..0x87` | 16 | 4 records | Canonical current controller records. |
| `tail + 0x88..0x8b` | 4 | 2 x u16be | Port-B controller-reset and note-toggle channel maps. |
| `tail + 0x8c` | 1 | packed u8 | Effect 4..6 connection in bits 2..0. |
| `tail + 0x8d..0x90` | 4 | bytes | Current left A/D destinations and levels, alternating. |
| `tail + 0x91` | 1 | s8 | Right A/D Pan. |
| `tail + 0x92..0x95` | 4 | bytes | Current right A/D destinations and levels, alternating. |
| `tail + 0x96..0xa5` | 16 | u8 values | StepWave values. |
| `tail + 0xa6` | 1 | packed u8 | Step count selector in bits 2..0; slope in bits 4..3; upper bits preserved. |
| `tail + 0xa7..0xaf` | 9 | bytes | Preserved opaque terminal bytes. |

Program controller records are 4-byte rows: `device_u8`, `function_u8`,
`type_u8`, and signed `range_s8`.

### Program Effect Blocks

The first three physical effect blocks start at `0x098`, `0x0c0`, and `0x0e8`.
Current records additionally store blocks four through six at `tail`,
`tail + 0x28`, and `tail + 0x50`. Legacy records have only three physical
blocks. Each block is `0x28` bytes; all sixteen parameters remain unsigned
16-bit words throughout decode and display, without byte truncation.

| Block offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `+0x00` | 1 | u8 | active_or_bypass_u8 |
| `+0x01` | 1 | u8 | input_level_u8 |
| `+0x02` | 1 | u8 | output_level_u8 |
| `+0x03` | 1 | s8 | pan_s8 |
| `+0x04` | 1 | u8 | output_u8 |
| `+0x05` | 1 | s8 | width_raw_s8 |
| `+0x06` | 1 | u8 | Current effect type. |
| `+0x07` | 1 | u8 | Legacy effect type projection. |
| `+0x08` | 32 | 16 x u16be | effect parameter words |

`width_display` is calculated as `width_raw_s8 + 63` when the result is in the
accepted display range.

Legacy type decoding uses `+0x07`. Selector `1` maps stored types `47..51`
to zero and subtracts five from types `52` and higher. Raw bytes are retained.
Current ordinary type values are `0..96`; the complete semantics of raw `97`
are not specified here. Preserve unknown types and their parameter words.
Effect words may represent numeric values, actions or unused slots; a numeric
scale is meaningful only for a numeric parameter. Changing an effect type
reinitializes all sixteen words. Bypass and reselecting the same type do not
reset those words.

### Program Assignment Rows

Program assignment rows start at `0x120` and use a `0x38` byte stride.

Only counted rows are assignments. Unused capacity and the terminal block
are not extra rows. Empty counted rows keep their ordinal positions. Clearing
a row need not change the count, shift subsequent rows or shrink the file.

| Row offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `+0x00` | 16 | ASCII | assignment_name |
| `+0x10` | 4 | u32be | Opaque source-local assignment handle. |
| `+0x14` | 1 | u8 | assigned_object_type |
| `+0x15` | 1 | u8 | midi_receive_channel_assign |
| `+0x16` | 1 | s8 | level_offset |
| `+0x17` | 1 | s8 | velocity_sensitivity_offset |
| `+0x18` | 1 | s8 | pan_offset |
| `+0x19` | 1 | s8 | velocity_xfade_high_offset |
| `+0x1a` | 1 | s8 | fine_tune_offset |
| `+0x1b` | 1 | s8 | velocity_xfade_low_offset |
| `+0x1c` | 1 | s8 | coarse_tune_offset |
| `+0x1d` | 1 | s8 | Current output1 replacement; -1 inherits. |
| `+0x1e` | 1 | u8 | key_limit_high |
| `+0x1f` | 1 | u8 | key_limit_low |
| `+0x20` | 1 | s8 | key_range_shift |
| `+0x21` | 1 | u8 | velocity_limit_high |
| `+0x22` | 1 | u8 | velocity_limit_low |
| `+0x23` | 1 | u8 | portamento_mono_key_xfade_flags |
| `+0x24` | 1 | s8 | Alternate group replacement; -1 inherits. |
| `+0x25` | 1 | s8 | aeg_attack_rate_offset |
| `+0x26` | 1 | s8 | aeg_decay_rate_offset |
| `+0x27` | 1 | s8 | aeg_release_rate_offset |
| `+0x28` | 1 | s8 | Current output2 replacement; -1 inherits. |
| `+0x29` | 1 | s8 | filter_cutoff_offset |
| `+0x2a` | 1 | s8 | filter_gain_offset |

Named kind-`0x10` and kind-`0x11` rows carry a source-local transient handle,
not a portable object identity. Zero is valid for that handle when resolving
a named assignment. No corresponding rule is specified for other row kinds.

| Row offset | Size | Type | Field |
| --- | ---: | --- | --- |
| `+0x2b` | 1 | s8 | filter_q_width_offset |
| `+0x2c` | 1 | s8 | cutoff_distance_offset |
| `+0x2d..0x2e` | 2 | bytes | Legacy output1 destination/level projection. |
| `+0x2f` | 1 | s8 | Current output1_level_offset. |
| `+0x30..0x31` | 2 | bytes | Legacy output2 destination/level projection. |
| `+0x32` | 1 | s8 | Current output2_level_offset. |
| `+0x33` | 1 | u8 | midi_control_on |
| `+0x34` | 1 | bits | Selection/isolation and unnamed state; preserve, not an authoring input. |
| `+0x35..0x37` | 3 | bytes | Preserved opaque row suffix. |

Assignment target kinds:

| Kind byte | Target category |
| ---: | --- |
| `0x10` | `SBNK` direct assignment target |
| `0x11` | `SBAC` Sample Bank target |

Rch Assign display family:

| Channel byte `+0x15` | Display |
| ---: | --- |
| `0xff` | `=Smp` |
| `0x00..0x0f` | `A01` through `A16` |
| `0x10` | `Bch` |
| `0x11..0x20` | `B01` through `B16` |
| any other value | `unknown` |

Output 2 at `+0x28` is independent of assignment enablement and receive-channel
selection. A non-`0xff` Output 2 value does not disable the assignment.

Assignments use the complete stored 16-byte name, target type and local scope.
Missing or ambiguous targets leave unresolved stored rows; they do not justify
redirecting an assignment to a similar name or another volume. The complete
device lookup behavior outside these cases is not specified.

For CD-ROM source loading, a named row can match an object in the same source
folder even when its stored target type differs from the source object's type.
This is a source-load case, not a general relaxation of ordinary assignment
matching. The receive selector keeps the display mapping above.

### System Receive Context

Global receive settings and Single/Multi setup are not PROG fields. They are
stored separately in [SYSTEM/SYSTEM2](system-files.md). In Multi mode the part's
receive channel takes precedence over the Sample or Sample Bank Rch Assign
setting inside its Program.

### Assignment Names And Duplicate Samples

Assignment target matching preserves the complete 16-byte sampler name. An
exact target whose stored name ends in `*` can be active and playable, so the
suffix alone does not classify a row as unresolved or off. If the exact target
is absent, keep the row unresolved rather than redirecting it to a similar
unstarred object.

In the sampler UI, `*` is not merely arbitrary punctuation: Yamaha's Duplicate
command creates a Sample named from the original name plus `*`. The new Sample
initially has the same parameters and shares the same Wave Data. The right-side
`E` indicator is transient edited-but-not-saved state. Relationship matching
still uses the exact stored target name; the suffix alone does not encode
unresolved, off, or duplicate state.

A `*` suffix alone therefore neither marks a missing target nor establishes
an independent Duplicate flag. Apply the same exact-name rules to starred
and unstarred objects.

## SEQU And PRF3

SEQU contains sequence timing and events; see [Sequence Data](sequences.md).
PRF3 includes the partition-level [System Files](system-files.md). Other PRF3
inner layouts are unspecified; do not apply the SYSTEM layout based on the
type tag alone.

## Object Relationships

Programs assign Samples or Sample Banks. Sample Banks contain Samples. A Sample
references one or two Wave Data members. Names and scope, not captured runtime
pointers, establish persistent relationships. Shared Wave Data does not merge
the parameters or playback windows of the Samples referencing it.

For software-facing representations see [Report Schemas](report-schemas.md),
[Export Layout](names-and-paths.md#exact-export-layout), and the
[Sample](sample-parameters.md) and [Program](program-parameters.md) authoring
contracts.
