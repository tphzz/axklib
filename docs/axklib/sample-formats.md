---
title: A-Series Sample Formats And Generations
---

# A-Series Sample Formats And Generations

An A-series Sample's stored format, its parameter values, the sampler's system
version and its installed hardware are separate compatibility questions. Choosing a format does
not certify a complete disk for a particular sampler.

## Stored Layouts

| Stored format | Sample parameter bytes | Sample header revision | Native generation |
| --- | ---: | ---: | --- |
| `a3000_188` | 188 | 2 | A3000 |
| `a4000_a5000_224` | 224 | 4 | A4000/A5000 |

The later block consists of a 188-byte prefix followed by a 36-byte extension.
The common physical prefix is not a universal semantic subset: some ranges are
wider on A3000, some packed bits change meaning, and some prefix fields stop being
authoritative on the later generation. File allocation and padding do not select
the format. See [A-Series Sampler Object Structures](sampler-data.md#sample-parameter-window)
for header lengths and byte locations.

Sample Banks store the same generation's parameters, but their object layout
also contains a member table. Native banks have a prefix-only parameter block;
later banks place the additional 36 bytes after that table. Their total object
size therefore depends on member capacity, not only parameter-block length.

## Parameter Differences

Ranges below describe stored parameter domains, not a promise that every system
revision exposes every setting. Parameter-relative offsets start at the Sample
parameter block, not the beginning of the file.

| Setting | A3000 format | A4000/A5000 format |
| --- | --- | --- |
| Coarse tune | `-127..127` | `-64..63` |
| Pitch bend type | `0..13` | `0..12` |
| Amplitude EG attack mode | `0..1` | `0..2` |
| Controller device / function | `0..125` / `0..21`, prefix records | `0..126` / `0..36`, extension records authoritative |
| Controller type / range | `0..3` / `-63..63` | Same domains |
| Sample EQ | Peak/Dip; no stored EQ mode selector | Peak/Dip, Low shelf or High shelf; mode bits are in the prefix |
| Velocity crossfade | Boolean at prefix `+0x29`, bit 3 | Independent low/high widths in extension `+0xd4/+0xd5` |
| Sample portamento | Off / `=Pgm` at prefix `+0x29`, bit 0 | Six modes at `+0xda`, with rate/time at `+0xdb/+0xdc` |
| Main / Output 1 destination | `0..4`, prefix `+0xa5` | `0..12`, extension `+0xd6` |
| Assignable / Output 2 destination | `0..5`, prefix `+0xa7` | `0..12`, extension `+0xd8` |

The prefix output levels are at `+0xa6/+0xa8`; later authoritative levels are at
`+0xd7/+0xd9`. All have the domain `0..127`.

Later objects retain the native controller records. Changing a later controller
updates its prefix projection; Function values above 21 project as zero. Other
controller fields copy unchanged, so even a projected record need not satisfy
all A3000 ranges. Retained prefix switches do not replace the authoritative
later portamento and crossfade fields. Preserve unrelated stored bytes rather
than treating superseded fields as writable padding.

## Outputs And Effects

The A3000 has effects and assignable outputs. The main generation difference is
the routing groups' destination choices, not the existence of those facilities.

| Native value | Main output | Assignable output |
| ---: | --- | --- |
| 0 | Off | Off |
| 1 | StereoOut | AssgnOut L&R |
| 2 | Effect 1 | AssgnOut 1&2 |
| 3 | Effect 2 | AssgnOut 3&4 |
| 4 | Effect 3 | AssgnOut 5&6 |
| 5 | Not in this group | DIG&OPT |

A3000's additional numbered output pairs and digital/optical outputs depend on
the optional AIEB1 expansion. Such hardware requirements do not make those native
destination values later-format fields. On the later generation both output
groups offer a broader destination set, with different numeric orderings; see
[Output Destinations](sampler-data.md#current-sample-output-destinations).
Effect 4, 5 and 6 destinations require A5000, even within the shared later format.

## System Versions And Playback

A3000 V2 adds features without introducing the later 224-byte stored layout.
Its ten additional filter types bring the available filter types to sixteen;
the bypass selector is separate. V2 settings are not guaranteed to reproduce on
V1. See Yamaha's [A3000 V2 Upgrade Guide](https://usa.yamaha.com/files/download/other_assets/9/328709/A3000V2E.pdf).

The A3000 parameter authoring domain includes V2-era settings. An `a3k` badge
therefore does not promise V1 playback compatibility. Matching filter names or
selectors between generations does not establish identical DSP response, timing
or sound. Physical playback testing remains distinct from stored-format checks.

## Authoring And Editing

Fresh Sample and Sample Bank specifications accept `storage_format` alongside
their names and parameters. Both values above are supported; omitting the field
selects the later profile. Unsupported parameter values reject the operation;
they do not trigger an implicit conversion. See [A-Series Sample Parameter Authoring](sample-parameters.md).

In axkdeck audio import, the **Sample format** selector applies to every Sample
and optional new Sample Bank in that batch. Each application launch starts with
**a3k**; subsequent batches remember the latest choice for that session only.
Import different formats in separate batches. Choosing the format does not alter
the imported PCM, sample rate, names or mappings. Existing packaged objects
retain their stored format instead of inheriting this audio-import preference.

The `a3k` and `a4k/a5k` badges identify stored format. Ordinary editing preserves
it. Explicit conversion changes that identity even when all parameters also have
an equivalent in the other generation. Conversion checks representation and
blocks loss; it does not certify hardware compatibility or change other objects
in the image. For A3000 media authoring, select native format and test the complete
media on the intended system version and installed hardware.
