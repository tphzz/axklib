# Audio

Audio APIs decode exact current `SMPL` waveform payloads and export WAV files.
Scoped export writes physical `SMPL` WAVs, rendered stereo WAVs when safe,
and in-memory selection graph data that carries musical relationships and quality.

Physical `SMPL` WAVs are always preserved as exact mono exports. Rendered stereo
is additive: compatible left/right material is written under `_samples/rendered/` and the
source `SMPL` files remain under `_samples/physical/`. Stereo render planning currently uses
two public patterns:

- one `SBNK` with known left and right `SMPL` member links;
- two known sibling `SBNK` objects in the same `SBAC` group whose sampler-facing
  names differ only by terminal `-L` and `-R`, each linked to its own physical
  `SMPL` object.

The second pattern covers discs that store stereo as paired sampler-visible
sample members rather than one object with an explicit right-member link. If
multiple sampler aliases point at the same physical left/right `SMPL` pair in one
volume, structured export writes one rendered WAV and lets graph metadata refer
to that shared artifact. When a rendered stereo stem would otherwise come from a
sampler duplicate marker such as a trailing `*`, axklib uses the owning
sample-bank or group label in the rendered stem when available. Physical `SMPL`
filenames still preserve exact storage-level names with filesystem-safe
sanitization, while graph JSON records sampler-visible aliases for
linked physical waveforms.

`WavExportRequest.progress_callback` receives export progress events for
physical WAV writes, rendered stereo writes, and graph writes. The
CLI uses the same progress stream to show the file currently being written.
`Waveform.alternating_byte_payload_detected` is true when a current-looking
`SMPL` object stores waveform bytes with the alternating-byte compatibility
pattern described in [Sampler Data Structures](sampler-data.md). The decoded WAV
uses the useful high-byte lane as 8-bit PCM, and the waveform is labeled with
`exactness_status = "alternating-byte-compatibility-export"` so callers can keep
that export path separate from ordinary current mono PCM.

::: axklib.audio
    options:
      members:
        - Waveform
        - WaveformPlacement
        - WaveformRelationship
        - WaveformIssue
        - WaveformSet
        - WavExportRequest
        - WavExportResult
        - decode_container_waveforms
        - export_waveforms
