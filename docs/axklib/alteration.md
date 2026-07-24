# Existing Image Alteration

The native alteration API alters an existing HDS image by writing a new image.
It never edits the source path in place. An alteration is a strictly ordered list
of typed operations; an `operation_ref` partition selector may refer only to an
earlier row and carries that row's resolved partition into the next operation.

Supported operations are:

- rename partition;
- insert, delete, and rename volume;
- insert, delete, and rename waveform;
- insert, delete, and rename Sample (`SBNK`);
- insert, delete, and rename Sample Bank (`SBAC`);
- insert and delete Program.

Wave Data insertion uses the same WAV, FLAC, and AIFF conversion pipeline as fresh
image creation. A subsequent Sample insertion in the same transaction can
reference the newly inserted Wave Data name. Stereo sources become two physical
mono Wave Data records when two `waveform_names` are supplied.

Deletion is conservative. A Sample cannot be deleted while a Program or
Sample Bank references it. Wave Data can be deleted only when exact
current-format ownership classifies it as known and unreferenced. Program and
Sample Bank operations require their raw assignments, membership flags, Program
bitmaps, and decoded relationships to agree.

## Object deletion planning

Interactive clients should use `inspect_object_deletion()` before deleting a
Sample Bank, Sample, or Wave Data object. The planner accepts one exact catalog
object and an explicit list of dependent objects to remove. It returns:

- blockers for incoming Program, Sample Bank, Sample, ambiguous, or
  allocation-inconsistent references;
- required, optional, preserved, and blocked object impacts;
- dependency prerequisites and relationship effects;
- estimated reclaimed allocation bytes and clusters (distinct from each
  object's logical stored size); and
- a typed alteration manifest ordered from Sample Bank to Sample to Wave Data.

Dependent cleanup is never implicit. Deleting a Sample Bank leaves its member
Samples as standalone objects unless the caller explicitly includes them.
Deleting a Sample likewise preserves its now-unreferenced Wave Data unless the
caller includes every safe Wave Data dependency. Direct Wave Data deletion is
available only for exact current-format objects classified as
`known_unreferenced`. The apply path replans against the retained image revision
before executing the typed manifest.

## Native API

`AlterationManifest` stores `AlterationOperationData`, a `std::variant` with one
public type per operation. `inspect_hds_alteration()` validates and executes the
complete queue against an in-memory mutable snapshot without writing output.
`alter_hds()` performs the alteration and requires an output path.

```cpp
auto manifest = axk::load_alteration_manifest("transaction.json");
if (!manifest) {
  return report(manifest.error());
}

auto inspection = axk::inspect_hds_alteration("source.hds", *manifest);
if (!inspection) {
  return report(inspection.error());
}

auto result = axk::alter_hds(
    "source.hds", *manifest, std::filesystem::path{"result.hds"});
if (!result) {
  return report(result.error());
}
```

Use an `operation_context` for cancellation and progress during long-running
jobs. Cancellation before publication removes the temporary output. The SDK's
stateless `alteration::inspect()` and `alteration::apply()` methods expose the
same inspection and direct-apply operations through the C++17 facade.

## Publication guarantees

Application uses a uniquely named sibling temporary file. Before publication,
the library verifies the exact planned record set, changed and inserted payloads,
root directory, and allocation bitmap. It flushes the temporary file to disk,
publishes without replacing an existing destination, and synchronizes the parent
directory where the platform provides that operation.

Fragmented records use continuation-list clusters when more than four extents
are required. Payload extents and list clusters are both included in allocation
and free-space accounting.
