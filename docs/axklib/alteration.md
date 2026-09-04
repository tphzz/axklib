# Existing Image Alteration

The native alteration API alters an existing HDS image by writing a new image.
It never edits the source path in place. An alteration is a strictly ordered list
of typed operations; an `operation_ref` partition selector may refer only to an
earlier row and carries that row's resolved partition into the next operation.

Supported operations are:

- rename partition;
- insert, delete, and rename volume;
- insert, delete, and rename waveform;
- insert, delete, rename, and update parameters on a Sample (`SBNK`);
- insert, delete, and rename Sample Bank (`SBAC`);
- assign selected Samples to an existing Sample Bank (`SBAC`);
- insert, delete, and rename Program.

Wave Data insertion uses the same WAV, FLAC, and AIFF conversion pipeline as fresh
image creation. A subsequent Sample insertion in the same transaction can
reference the newly inserted Wave Data name. Stereo sources become two physical
mono Wave Data records when two `waveform_names` are supplied.

Deletion is conservative. A Sample cannot be deleted while a Program or
Sample Bank references it. Wave Data can be deleted only when exact
current-format ownership classifies it as known and unreferenced. Program and
Sample Bank operations require their raw assignments, membership flags, Program
bitmaps, and decoded relationships to agree.

`assign_sbac_members` moves one through 127 named Samples into one existing
Sample Bank in the same partition and volume. Members already in the target
retain their current row and order. Other selected Samples are detached from
their previous Sample Banks and appended in request order; source banks remain
present and may become empty. The target Sample Bank keeps its SFS identity, so
Program assignments to it remain valid. A Sample assigned directly to a Program,
a shared or inconsistent membership, a final count above 127, or target payload
growth beyond the bank's currently allocated record extents rejects the complete
transaction without changing the image. Appending rows consumes existing slot
padding first and preserves the target record's opaque suffix bytes.

`update_sbnk_parameters` applies a non-empty partial
[`SampleParameters`](sample-parameters.md) object to one existing Sample.
Fields omitted from the update and unrelated opaque bytes are preserved. The
same validator and byte encoder are used for fresh Samples and existing-object
updates; derived caches are recomputed from changed source values. Sample Bank
`parameter_overrides` uses this model too, prepares every member update before
mutation, and leaves the bank's pending-propagation bits clear.

## Object deletion planning

Interactive clients should use `inspect_object_deletion()` before deleting a
Program, Sample Bank, Sample, or Wave Data object. The planner accepts exact
catalog targets plus an explicit list of optional dependent objects to remove,
with a combined limit of 1,024 selected inputs. Targets may span volumes and
partitions. It returns:

- blockers for incoming Program, Sample Bank, Sample, ambiguous, or
  allocation-inconsistent references;
- required, optional, preserved, and blocked object impacts;
- dependency prerequisites and relationship effects;
- estimated reclaimed allocation bytes and clusters (distinct from each
  object's logical stored size); and
- a typed alteration manifest ordered within each partition and volume from
  Program to Sample Bank to Sample to Wave Data.

Each target is evaluated against the whole requested batch. A target blocked by
an unselected reference remains unchanged, while every independently eligible
target can still be submitted as one atomic alteration. A batch with no
eligible target cannot be applied.

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
root directory, and both complete SFS allocation bitmap copies. It independently
reopens the temporary image and requires both stored copies to match each other
and all reconstructed index extents. It flushes the temporary file to disk,
publishes without replacing an existing destination, and synchronizes the parent
directory where the platform provides that operation.

The same allocation-integrity predicate gates every mutation entry point. An
image with divergent bitmap copies, stored/index disagreement, invalid extents,
extent-total disagreement, or cross-linked ownership remains available for
browsing, validation, and export, but volume, partition, object, and package
mutations are disabled. axkdeck opens its Image integrity dialog automatically
for these blocking conditions and also exposes it from the image menu.

Axklib has one deliberately narrow repair operation for an otherwise clean SFS
image whose record extent byte totals differ from the records' logical sizes.
It normalizes every such record in one copy-only transaction, updates both
allocation bitmap copies when a wholly trailing extent can be released, then
reopens the output and requires complete allocation agreement and byte-identical
logical payloads. The source image is never overwritten. Axkdeck exposes this as
`Repair copy...` in the Image integrity dialog when, and only when, the complete
image satisfies that repair contract.

Other allocation failures are not repaired automatically. In particular, a
cross-linked cluster does not contain enough information to determine which
owner should keep possibly overwritten bytes. Divergent bitmap copies,
stored/index disagreement, invalid extents, ambiguous underreported
multi-extent records, and cross-links remain read-only. Preserve the source
image, export every still-readable object, and continue from a known-good backup
or a newly authored image rather than guessing allocation ownership.

Fragmented records use continuation-list clusters when more than four extents
are required. Payload extents and list clusters are both included in allocation
and free-space accounting.

## Sampler-retained directory tombstones

Sampler-authored deletion can leave a directory row whose link ID has a `0xF`
high nibble. The reader preserves this row as a deleted slot, but it is not a
live object and does not cause a destination-name conflict.

Insertion reuses these slots deterministically: a same-name tombstone is chosen
first, otherwise the deleted row with the lowest payload offset is used. The
complete 32-byte row is overwritten. Reusing a row adds no directory-payload
bytes, and package-import planning uses that same net-growth rule for root,
volume-category, and object-directory capacity. Tombstones unrelated to the
insertion remain byte-for-byte unchanged.

Axklib's own deletion operations continue to remove and compact directory rows;
they do not create new tombstones. A live directory row whose target record is
missing is not reusable deletion history: validation reports
`SFS_DIRECTORY_ENTRY_TARGET_MISSING`, and the normal mutation integrity gate
keeps the image read-only.
