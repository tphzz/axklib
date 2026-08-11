# Report Schemas

Commands that emit CSV or JSON write a schema index beside their reports. Each
schema has a major and minor version. Consumers must reject an unsupported major
version and may ignore unknown fields added in a compatible minor version.

Object rows include container kind, scope, object key, sampler object type and
name, payload location, decoded kind, and diagnostic fields. Relationship rows
include source, target or candidates, relationship type, quality, basis,
assignment state, and sampler-facing location. Validation rows include stable
issue code, severity, message, sampler path, and technical object key.

Exact exports also write a selection graph that links Wave Data files, rendered
stereo files, Samples, Sample Banks, Programs, parameters, and any
unresolved decisions.
