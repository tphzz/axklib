# ISO Volume Relationship Locality

## Source

- Image: `A3000 Factory CDROM/A3000 ProSnd Lib.iso` beneath a user-supplied
  external corpus root
- Size: `66,789,376` bytes
- SHA-256: `dd4e8544f84bbd0d8ed22d70f05873465c0581b10d060b9c99f85ce1cc1921d1`
- Checked: 2026-07-27

The ISO remains in the external read-only corpus. No binary copy is retained in
the repository.

## Observations

**Strong:** ISO object names are not globally authoritative relationship keys.
The same disc-wide catalog scope contains objects from multiple sampler
volumes, while the ISO directory containing an object identifies its local
volume.

The physical Sample Bank `B Conga` and its six Samples are stored in volume
`13 Perc /5.1M`. An A3000 hardware check confirmed that `B Conga` is not visible
after loading volume `14 S.E.    /2.3M`. A name match in another ISO volume must
therefore remain an unresolved candidate rather than becoming a navigable or
contained relationship.

The repeated spaces in the sampler-visible volume name
`14 S.E.    /2.3M` are significant presentation data and must not be collapsed
by axkdeck.

## Reproduction

```sh
AXK_CORPUS_ROOT=/path/to/yamaha-sample-cdroms
build/native/release/apps/cli/axklib info \
  --format json \
  "$AXK_CORPUS_ROOT/A3000 Factory CDROM/A3000 ProSnd Lib.iso" \
  > build/reports/00034_a3000_prosnd_iso_locality/info.json
```

The current report contains the physical bank at:

```text
A3000 ProSnd Lib/13 Perc _5.1M/Sample Banks and Samples/B Conga
```

It contains no `Conga` child beneath volume `14 S.E. _2.3M`. The underscore in
the selector path is the sanitized path representation of `/`; the content
node's `display_name` retains the exact sampler-visible spacing and slash.

Automated regressions use synthetic catalogs and a generated HDS fixture so
they remain self-contained and do not depend on this external ISO.
