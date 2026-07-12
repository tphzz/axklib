# C++ API

All public declarations are under `cpp/include/axklib` and namespace `axk`.
Operations that can fail return `axk::Result<T>`, an `std::expected`-based type
with a stable error category and native error code.

| Header | Main responsibility |
| --- | --- |
| `media.hpp` | Detect and open SFS, FAT12, ISO9660, and standalone inputs |
| `catalog.hpp` | Decode objects and sampler-facing placement |
| `relationship.hpp` | Build Program, bank-group, sample-bank, and waveform links |
| `semantic.hpp` | Content trees, orphan classification, and validation |
| `audio.hpp` | Decode physical waveforms and preview envelopes |
| `audio_export.hpp` | Exact WAV, rendered stereo, and structured export plans |
| `writer.hpp` | Load manifests and create fresh HDS images |
| `alteration.hpp` | Plan and apply ordered existing-image transactions |
| `effects.hpp` | Effect type and parameter metadata |

Readers use bounded random-access I/O and accept a cancellation token. Immutable
snapshots may be read concurrently. Mutation plans are single-owner values and
must be applied in their declared order.

The C++ API follows semantic versioning. Source-compatible additions can occur
within a major release; incompatible declarations require a major version.
