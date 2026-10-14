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

Axkdeck selects source members in Open and native documents in Device view.
A-series keeps its editors; TX16W has read-only object browsing, typed details,
source locations and missing/ambiguous relationships. Document switches isolate
presentation state and reject stale replies. The existing TX16W import workflow
is unchanged. Retained conversion plans and native TX16W writing are not supplied
by this integration.
