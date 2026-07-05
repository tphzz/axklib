# Audio

Audio APIs decode exact current `SMPL` waveform payloads and export WAV files.
Structured export writes physical `SMPL` WAVs, rendered stereo WAVs when safe,
and one per-volume graph JSON that carries musical relationships and quality.

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