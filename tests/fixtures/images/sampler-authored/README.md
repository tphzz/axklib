# Sampler Image Fixtures

Small Yamaha A-series images used by active automated tests. These are
versionable test fixtures, not scratch outputs and not large corpus images. Keep
this folder minimal; replace these binaries with smaller synthetic/minimal
fixtures when practical.

## HD00_512_multi_sbnk_authored.hds

- Bytes: 1048576
- SHA-256: `733b25eb1b9da4b155ec543f48e0c0f3ab5480dbba587ee80fea5df40dfa0d7d`
- Purpose: multi-slot current SBNK fixture for active public tests
- Origin: sampler-authored A-series image retained from the pre-public
  multi-slot hardware campaign; the exact authoring transcript is not retained
- Quality: known small test dependency
- Retention reason: required by active automated tests; replace with a smaller synthetic/minimal fixture when practical
- Expected object count: see `MANIFEST.json`
- Regeneration note: see `MANIFEST.json`
- Duplicate-family structure: ten standalone Samples span zero through nine
  trailing stars and share one Wave Data object; ten Sample Bank/Sample pairs
  span zero through eight and ten trailing stars and share a second Wave Data
  object. Within each family the decoded Sample parameters are identical.
- Verification status: this structure, the Yamaha manual, later hardware edits
  and saves, and operator recollection support repeated Duplicate operations.
  The missing original authoring transcript prevents a definitive claim about
  the exact command sequence or any unidentified raw Duplicate field.
- Audio limitation: grouped `Sxx` members reference `SMP 252511`, whose exact
  PCM has near-zero amplitude. Use this fixture for structure and parameter
  tests, not as an audible grouped-bank control. Standalone `JSxx` rows
  reference the audible `pulse 1` waveform.

## HD00_512_single_sbnk_authored.hds

- Bytes: 1048576
- SHA-256: `9604f208c23360f2b69c78bc7ae94cf9f9c12cf96ffddb3567dfadd8200cc816`
- Purpose: single-member current SBNK/SMPL fixture for active public tests
- Origin: curated public test fixture
- Quality: known small test dependency
- Retention reason: required by active automated tests; replace with a smaller synthetic/minimal fixture when practical
- Expected object count: see `MANIFEST.json`
- Regeneration note: see `MANIFEST.json`
