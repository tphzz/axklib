# Native Fuzzing

Fuzzing is a C++ memory-safety and parser-hardening tool. It is not inherited
from the Python oracle and does not prove unknown sampler-field semantics.
`cpp/fuzz/harnesses.json` is the canonical inventory of targets, production
entry points, synthesized envelopes, input bounds, seed directories, and
invariants. Normal tests replay every maintained seed through the same envelope.

## Execution Tiers

The `fuzz-local` preset builds every target with Clang, libFuzzer, ASan, and
UBSan, copies immutable seeds below the ignored build tree, then runs each
target for five seconds. Override `AXK_FUZZ_SMOKE_SECONDS` at configure time for
a longer local smoke. `clang` and `clang++` must be available on `PATH`; the
preset fails at configuration when that prerequisite is absent.

```bash
cmake --preset fuzz-local
cmake --build --preset fuzz-local
```

For an extended campaign, configure the preset, then invoke one binary directly
against a copied working corpus with an explicit duration:

```bash
cmake -E make_directory build/native/fuzz-local/artifacts
build/native/fuzz-local/cpp/axk_sfs_image_fuzz \
  build/native/fuzz-local/fuzz-corpus/sfs_image \
  -max_total_time=14400 -timeout=5 -rss_limit_mb=2048 \
  -artifact_prefix=build/native/fuzz-local/artifacts/sfs_image-
```

Store transient logs under `build/logs/fuzz/`. A retained run summary records
the commit, compiler, sanitizers, target, corpus hash, duration, executions,
coverage/features, peak RSS, slowest unit, and every crash/hang/OOM result. The
short smoke tier must not be described as an extended campaign.

## Recorded Extended Campaign

The 2026-07-12 SFS image campaign ran four independent workers from
`14:03:46Z` through `15:03:47Z`. The pass-20 source tree was based on revision
`1b839dc6105984e9b054674afee6959f351ecc2a`; the executed
`axk_sfs_image_fuzz` SHA-256 was
`e3d8bd31220abfc4895a720c3da89eb1b83adfebd8a4626c879a0a88ae2521c5`.
It used Debian Clang 19.1.7 with libFuzzer, AddressSanitizer, and
UndefinedBehaviorSanitizer.

All workers started from the same 25-file, 11,459,089-byte post-smoke corpus.
The SHA-256 of the sorted `file SHA-256 + filename` manifest was
`4931a183a64a128f10181aea7b2b04d3bdd91411d27f8b0788ebdc1d16c028a7`.
Each worker used a 3,600-second budget, a five-second per-input timeout, a
2,048 MiB RSS limit, and a configured 4 MiB maximum input. The largest input
limit reached during this run was 1,049,016 bytes; the table does not imply
that 4 MiB inputs were reached.

| Worker | Executions | Final coverage | Final features | New units | Peak RSS | Slowest unit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 90,843 | 878 | 3,370 | 243 | 456 MiB | <1 s |
| 2 | 101,923 | 878 | 3,326 | 190 | 432 MiB | <1 s |
| 3 | 92,440 | 878 | 3,449 | 213 | 451 MiB | <1 s |
| 4 | 88,582 | 878 | 3,330 | 197 | 443 MiB | <1 s |

The aggregate was 373,788 executions over 14,404 worker-seconds. No crash,
hang, timeout, OOM, ASan report, UBSan report, or artifact was produced. This
result covers malformed-input robustness for the current `open_image` SFS/HDS
path; it does not establish sampler-format meaning or writer correctness.

## Promotion

Minimize a finding before promotion. Add a descriptive `.seed` under the exact
target corpus, record its origin without private image bytes, and add a
deterministic normal-test assertion. Hash-named mutable corpus files, private
real images, and raw crash directories are not versioned.
