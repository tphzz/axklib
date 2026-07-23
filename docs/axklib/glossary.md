# Yamaha Glossary

axklib keeps Yamaha object type strings separate from sampler-facing names. The
terms below describe how objects appear in reports, `info` output, axkdeck, and
structured export graphs.

## Object Terms

- **Program (`PROG`)**: Program object containing assignment rows and
  Program-level parameters. User-facing trees render its slot and display name,
  such as `001: TSUYOSHI`, or a synthesized default such as `002: Pgm 002`.
- **Sample Bank (`SBAC`)**: Sampler-visible parent containing Sample (`SBNK`)
  member entries. This is the only object level rendered with the `B ` prefix,
  for example `B TS-KICK`. `SBAC` is a raw Yamaha type string, not an acronym
  expanded in user-facing text.
- **Sample (`SBNK`)**: Sampler-visible Sample object. A Sample can be assigned
  directly to a Program or appear as an unprefixed member below a Sample Bank.
  It stores Sample parameters and links to one or two Wave Data objects. Never
  render an `SBNK` Sample with the `B ` prefix.
- **Wave Data (`SMPL`)**: Physical storage object containing PCM and Wave Data
  metadata. Its storage name can differ from the sampler-visible Sample name.
- **Sequence (`SEQU`)**: Sequence object, listed separately from Program and
  sample data when present.

## Runtime Quality Labels

Reports and structured APIs serialize relationship and decoded-value quality as:

- **Known**: Available metadata identifies one authoritative value or target.
- **Likely**: Available metadata identifies one probable value or target, but
  does not meet the `Known` threshold.
- **Tentative**: One or more candidates are retained for inspection without an
  authoritative resolution.
- **Unknown**: No supported interpretation or target is available.

## Export Terms

- **Physical WAV**: Exact mono WAV written from one decoded Wave Data (`SMPL`)
  payload into the shared `_samples/physical/` pool.
- **Rendered WAV**: Derived audio, such as compatible left/right Wave Data
  interleaved for an owning Sample (`SBNK`), written to
  `_samples/rendered/`.
- **Volume graph**: Current `volume.axklib.json` metadata written per exported
  volume. It links Programs, Sample Banks, Samples, Wave Data, physical WAVs,
  rendered WAVs, parameters, and relationship quality.
- **Unresolved Wave Data graph**: Metadata for decodable Wave Data without one
  authoritative volume placement. It retains placement candidates, quality,
  and the shared physical WAV reference rather than hiding the object.
- **Selection graph**: The volume and unresolved graph records produced for the
  requested extraction scope.
