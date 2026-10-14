# Native Sampler Modules

Filesystem access, native sampler semantics, conversion and application
orchestration are separate responsibilities. A sampler is not a filesystem:
several sampler formats can use the same filesystem, and one native document
can span several explicitly selected source members.

## Module Responsibilities

- Source admission owns storage identity, bounded readers and source lifetime.
- Filesystem adapters expose directories, attributes and file contents without
  interpreting sampler objects.
- Native format modules own recognition, typed records, object relationships
  and format-specific capabilities. PCM is read on demand rather than retained
  in every catalog entry.
- Directional converters own mappings between specific native formats and
  report loss, uncertainty and unsupported features.
- Application sessions own authorization, revisions, jobs and publication.
  Format modules do not independently commit changes or retry uncertain writes.

Objects retain their storage occurrence identity. Equal names or bytes do not
merge separate objects. Missing and ambiguous references remain explicit;
conversion eligibility is a separate decision from native browsing.

Conversions prioritize explicitly supported directions and fidelity. The design
does not require a universal sampler model, an automatic chain of converters or
a runtime plugin system. Generic HTTP resources can expose format-specific
details without introducing separate endpoints for every sampler.

## Implementation Scope

The initial engine increment provides neutral source and native-document
contracts, a FAT file-view adapter and a read-only TX16W document implementation.
TX16W decoding is separated from its A-series conversion policy. The installed
SDK remains a C++17 facade; internal engine headers are not installed APIs.

A-series inventory, relationships and validation now share an immutable format
owner. Its application adapter owns native projections, details, capabilities,
PCM interpretation and companion recovery. Common sessions retain leases,
revision checks, mutation recovery and publication.

The server admits explicit source sets and exposes document-scoped native reads
and operations. Files remains shared across the source set; each entry identifies
its source member and any native document bindings. Physical devices remain
exclusive single-member sources. A native write targets one writable member
through the common revision and recovery path.

Axkdeck admits source members in Open, then displays one image at a time. The
filename selector scopes Files roots, navigation, selection and diagnostics to
that member. Device uses its uniquely associated native document; ambiguous
interpretations have a separate format chooser. Companion images remain
individually selectable in Files while sharing their native Device document.
Files presentation state is retained per source and Device state per document.
Multiple identifies ownership of the entire set; Eject all images closes it.

A-series keeps its editors; TX16W has read-only object browsing, typed details,
source locations and missing/ambiguous relationships. Switches are guarded and
reject stale replies. TX16W conversion uses a retained directional plan; native
TX16W writing remains unsupported.

## Conversion Structure

Keep native decoding in `library/{include/axklib,src}/formats/<format>/` and
filesystem interpretation in `filesystems/<filesystem>/`. Do not place sampler
semantics in generic sessions or HTTP handlers.

- `conversion/plan.hpp` and `conversion/plan.cpp` define the common profile,
  report, immutable patch payload, fidelity and acknowledgement contract. They
  must not include concrete format adapters or application/transport code.
- `conversion/tx16w_to_a_series/` owns that direction only: native-document
  inspection, parameter mapping and its registered profile. A new high-fidelity
  direction belongs in its own sibling module, not a switch in common code.
- `formats/a_series/authoring/` accepts destination-native object bundles and
  prepared PCM. It owns A-series inventory, allocation and validation, and has
  no knowledge of TX16W or conversion profiles.
- `conversion/file_plan.*` and its private `file_sources.*` are the file-client
  composition and publication adapter. `sdk_conversion.cpp` wraps it behind a
  C++17 PIMPL. The CLI consumes the same file adapter, not the optional shared SDK.
- Application `conversion_sources`, `conversion_plan_store` and
  `conversion_operations` own admission, bounded retention and jobs. The shared
  `prepared_session_commit` owns journaled mutation publication for conversion
  and ordinary alterations alike.
- Axkdeck `HttpConversions` transports the generic contract. `ConversionReport`
  renders format-independent review fields and impacts; the TX16W workflow
  selects the profile and manages source/target UI state.

Adding a profile requires an explicit registration at each composition root,
an options schema, a native source and destination format ID, and bounded
read-only preparation. The prepared payload must contain everything required
to execute the reviewed result. Do not add an intermediate sampler model,
dynamic plugin ABI or per-sampler HTTP endpoints to support another direction.
See [Retained Directional Conversion](axklib/conversion.md) for lifecycle,
resource limits, public contracts and recovery behavior.
