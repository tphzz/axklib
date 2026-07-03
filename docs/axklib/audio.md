# Audio

Audio APIs decode exact current `SMPL` waveform payloads and export WAV files.
Structured export writes physical `SMPL` WAVs, rendered stereo WAVs when safe,
and one per-volume graph JSON that carries musical relationships and quality.

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