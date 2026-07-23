# CLI JSON Schemas

Machine-readable CLI output uses typed DTOs that are separate from sampler
domain models and from the implementation JSON library. JSON key spelling, field order,
null behavior, and pretty/compact rendering belong to the serializer named
below. Human renderers consume the same service results without parsing JSON.

| Output | Version | DTO and serializer owner |
| --- | --- | --- |
| `info --format json` | `compat-v1` (wire shape has no version field) | `cli/schema/info_v1.*` |
| `alter hds` stdout | `compat-v1` (wire shape has no version field) | `cli/schema/operations_v1.*` |
| `objects --format json` | `1.0` | `cli/schema/objects_v1.*` |
| `volume.axklib.json` | `axklib.volume_graph.v1` | `cli/schema/export_v1.*` |
| `unresolved.axklib.json` | `axklib.unresolved_wave_data.v1` | Application exact-export serializer |
| Inventory, validation, coverage, orphan, relationship, and corpus report JSON | Per-file schema sidecar | Typed `ReportRow` projection and `axklib/report.*` |

Missing fields, JSON null, empty strings, and empty arrays are distinct schema
states. Serializers retain integer widths and reject invalid internal UTF-8.
Adding or changing a machine field requires an edit in the owning DTO/serializer
and an exact or parsed golden test. Before the first supported release,
unreleased schema meanings are corrected in place with every in-repository
consumer updated in the same change. Versioned migrations begin only after a
released contract or persisted user data requires compatibility.

Object schema `1.0` applies the canonical object terminology: decoded `SBNK`
objects expose `sample_name`, their members expose `wave_data_name`, and decoded
`SBAC` objects represent Sample Banks.

nlohmann/json is an implementation dependency. It appears only in manifest or
export-graph input readers, report serialization, and the dedicated schema
serializer translation units. Installed headers and DTO headers do not expose
its types or exceptions.
