# Yamaha Glossary

axklib uses Yamaha object type strings for on-disk objects and sampler-facing names for navigation and exports. The terms below describe how those names appear in reports, `info` output, and structured export graphs.

## Object Terms

- **Wave Data (`SMPL`)**: Storage object containing PCM and current-format Wave Data metadata. Its stored name can differ from the sampler-visible Sample name.
- **Sample (`SBNK`)**: Sampler-visible Sample object shown below a `B <name>` Sample Bank or as a standalone Program assignment. It carries pitch, loop, key/velocity, envelope, filter, LFO, and output parameters and links to one or two Wave Data objects.
- **Sample Bank (`SBAC`)**: Sampler-visible `B <name>` parent containing Sample (`SBNK`) entries. `SBAC` is the raw Yamaha object type string and is not expanded as an acronym.
- **`PROG`**: Program object. Programs hold assignment rows and program-level parameters. In the sampler UI, programs are shown as slots such as `001: TSUYOSHI` or default names such as `Pgm 002`.
- **`SEQU`**: Sequence object. These are listed separately from sample and program data when present.

## Data Quality Labels

- **Known**: The field or relationship is stable enough for normal reporting and export decisions.
- **Likely**: The value is consistent across available inputs, but is less stable than `Known`.
- **Tentative**: The value is a candidate interpretation exposed for inspection.
- **Unknown**: Raw bytes or locations are known, but the meaning is not decoded.

## Export Terms

- **Physical WAV**: Exact mono WAV written from one Wave Data (`SMPL`) payload.
- **Rendered WAV**: Derived audio such as interleaved stereo assembled from known linked left/right `SMPL` objects.
- **Structured export graph**: The graph record used by export APIs, optional selection graph JSON, or older `volume.axklib.json` files to link Programs, Sample Banks, Samples, Wave Data, and rendered audio.
