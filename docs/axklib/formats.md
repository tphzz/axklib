---
title: Formats Overview
---

# Formats Overview

Format documentation describes three different layers:

- **Disk images and filesystems** define how files are located and stored on media.
- **Sampler objects and records** define the contents of those files for a particular device family.
- **Archives and packages** bundle content for transfer without reproducing a disk's filesystem.

A shared filesystem does not imply shared sampler data. For example, A-series
and SU700 hard disks both use SFS, but their sample and song files have different
layouts. Conversely, A-series object files can appear on several container types.

## Find Your Device Family

**A-series** means Yamaha A3000, A4000 and A5000. Model and system-version
differences are described on the individual format pages; a family heading does
not imply identical encodings or capabilities on every model.

| Device family | Disk images and filesystems | Sampler objects and records |
| --- | --- | --- |
| A-series | [SFS hard disks](sfs-filesystem.md), [FAT12 floppies](floppy.md), [ISO9660 CD-ROMs](cdrom.md) | [Object structures](sampler-data.md), [Sample formats and generations](sample-formats.md), [Sequence data (SEQU)](sequences.md), [System files](system-files.md) |
| SU700 | SFS hard disks and FAT12 floppies; see [file placement](su700.md#placement-and-names). The [SFS specification](sfs-filesystem.md#specification-scope) details the A-series formatted layout. | [File layout and sample data](su700.md), [songs and tracks](su700-song.md), [effects](su700-effects.md) |
| EX5 | [FAT16 hard-disk and removable-media layouts](ex5.md) | The disk-format reference does not specify EX5 sound or sequence payloads. |

For what axklib can read, write, import or export, use
[Supported Media Profiles](media.md). Container access alone does not establish
interpretation or editing of the files inside it.

## Archives And Packages

[A3K volume archives](a3k-archive.md) are a separate archive format for A-series
content. [AXK portable object packages](portable-packages.md) are axklib's own
package format, containing A-series object payloads and their relationship graph.
Neither should be confused with an SFS, FAT12 or ISO9660 disk image.

## Parameters And Operations

For stored bytes, start with the device-specific references above. For JSON
fields and operations, use [A-Series Sample Parameter Authoring](sample-parameters.md),
[A-Series Program Parameters](program-parameters.md),
[A-Series Sequence Transfer And MIDI Conversion](sequence-midi.md), and
[Writer And Alteration](write.md). The [Current Contract Index](current-contracts.md)
also connects these specifications to output and interface contracts.
