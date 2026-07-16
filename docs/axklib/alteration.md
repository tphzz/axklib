# Existing Image Transactions

The native transaction API alters an existing HDS image by writing a new image.
It never edits the source path in place. A transaction is a strictly ordered list
of typed operations; an `operation_ref` partition selector may refer only to an
earlier row and carries that row's resolved partition into the next operation.

Supported operations are:

- insert and delete volume;
- insert, delete, and rename waveform;
- insert, delete, and rename Sample (`SBNK`);
- insert, delete, and rename Sample Bank (`SBAC`);
- insert and delete Program.

Waveform insertion uses the same WAV, FLAC, and AIFF conversion pipeline as fresh
image creation. A subsequent sample-bank insertion in the same transaction can
reference the newly inserted waveform name. Stereo sources become two physical
mono waveform records when two `waveform_names` are supplied.

Deletion is conservative. A sample bank cannot be deleted while a Program or
Sample Bank references it. Wave Data can be deleted only when exact
current-format ownership classifies it as known and unreferenced. Program and
sample-bank-group operations require their raw assignments, group flags, Program
bitmaps, and decoded relationships to agree.

## Native API

`AlterationManifest` stores `AlterationOperationData`, a `std::variant` with one
public type per operation. `plan_hds_alteration()` validates and executes the
complete queue against an in-memory mutable snapshot without writing output.
`alter_hds()` uses the same planning path and optionally publishes the result.

```cpp
auto manifest = axk::load_alteration_manifest("transaction.json");
if (!manifest) {
  return report(manifest.error());
}

auto plan = axk::plan_hds_alteration("source.hds", *manifest);
if (!plan) {
  return report(plan.error());
}

auto result = axk::alter_hds(
    "source.hds", *manifest, std::filesystem::path{"result.hds"});
if (!result) {
  return report(result.error());
}
```

Use an `operation_context` for cancellation and progress during long-running
jobs. Cancellation before publication removes the temporary output. The SDK's
owned `transaction` value provides the plan/apply split.

## Publication guarantees

Application uses a uniquely named sibling temporary file. Before publication,
the library verifies the exact planned record set, changed and inserted payloads,
root directory, and allocation bitmap. It flushes the temporary file to disk,
publishes without replacing an existing destination, and synchronizes the parent
directory where the platform provides that operation.

Fragmented records use continuation-list clusters when more than four extents
are required. Payload extents and list clusters are both included in allocation
and free-space accounting.
