# Yamaha Glossary

axklib uses Yamaha object type strings for on-disk objects and sampler-facing names for navigation and exports. The terms below describe how those names appear in reports, `info` output, and structured export graphs.

## Object Terms

- **`SMPL`**: Physical waveform/storage object. This object contains the PCM payload and current-format waveform metadata. A `SMPL` object name can differ from the name shown for a playable sample in some sampler views.
- **`SBNK`**: Sample bank/member object. In sampler navigation this is often the visible child under a `B <name>` sample-bank group, or a standalone program assignment. `SBNK` carries member parameters such as pitch, loop, key/velocity, envelope, filter, LFO, output, and links to one or two `SMPL` waveform objects.
- **`SBAC`**: Sample-bank grouping object. The raw Yamaha object type string is `SBAC`. In `info` output, an `SBAC` commonly appears as the sampler-visible `B <sample bank>` parent that groups child `SBNK` entries.
- **`PROG`**: Program object. Programs hold assignment rows and program-level parameters. In the sampler UI, programs are shown as slots such as `001: TSUYOSHI` or default names such as `Pgm 002`.
- **`SEQU`**: Sequence object. These are listed separately from sample and program data when present.

## Data Quality Labels

- **Known**: The field or relationship is stable enough for normal reporting and export decisions.
- **Likely**: The value is consistent across available inputs, but is less stable than `Known`.
- **Tentative**: The value is a candidate interpretation exposed for inspection.
- **Unknown**: Raw bytes or locations are known, but the meaning is not decoded.

## Export Terms

- **Physical WAV**: Exact mono WAV written from one `SMPL` payload.
- **Rendered WAV**: Derived audio such as interleaved stereo assembled from known linked left/right `SMPL` objects.
- **Structured export graph**: The graph record used by export APIs, optional selection graph JSON, or older `volume.axklib.json` files to link programs, sample-bank groups, sample banks, physical waveforms, and rendered audio.
