# Existing Image Alteration

The native alteration API alters an existing HDS image by writing a new image.
It never edits the source path in place. An alteration is a strictly ordered list
of typed operations; an `operation_ref` partition selector may refer only to an
earlier row and carries that row's resolved partition into the next operation.

Supported operations are:

- rename partition;
- insert, delete, and rename volume;
- insert, delete, rename, and update metadata on Wave Data (`SMPL`);
- insert, delete, rename, retarget Wave Data, update parameters, and explicitly convert the stored format of a Sample (`SBNK`);
- insert, delete, rename, and update parameters on a Sample Bank (`SBAC`);
- assign selected Samples to an existing Sample Bank (`SBAC`);
- insert, delete, rename, update parameters, and replace assignments on a Program.

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
a shared or inconsistent membership, a final count above 127, or insufficient
free allocation rejects the complete transaction without changing the image.
Appending rows consumes existing slot padding first, grows allocation when
needed, and preserves the parameter tail and opaque suffix bytes.

`update_wave_data_parameters` changes only an existing Wave Data object's metadata.
Its target fields are `partition_index`, `volume_name`, and `waveform_name`.
The non-empty `parameters` object accepts `root_key` (0..127),
`fine_tune_cents` (-63..63), `loop_mode` (0..5), and unsigned frame values
`wave_start_frame`, `wave_length_frames`, `loop_start_frame`, and
`loop_length_frames`. Playback windows must fit the complete stored PCM and loops
must fit the playback window. Nonrepeating modes allow a zero start/length loop;
repeating modes require a nonempty loop. Pitch edits update their derived cache.
PCM8/PCM16 storage and unrelated metadata are preserved exactly. This does not
change the independent parameters of referencing Samples, resample audio,
change encoding, or admit incomplete/unsupported transfer profiles.

`update_sample_bank_parameters` applies a non-empty partial `parameters` object
to an existing Sample Bank and all its members atomically. Its target fields are
`partition_index`, `volume_name`, and `sample_bank_name`. It uses the same typed
Sample parameter contract, validates each member's merged values, and preserves
unrelated object bytes, Wave Data and relationship identities. Complete native
revision-2 banks and later split-tail banks with clear pending propagation state
are supported. Bank and member formats are retained and independently validated. Pending state,
unresolved or multiply-owned members, and invalid merged values reject the entire
transaction without publishing a partial change.

Fresh `insert_sbnk` Sample and `insert_sbac` Sample Bank specifications accept
`storage_format`: `a3000_188` or `a4000_a5000_224`. Omission selects the later
format. The bank's format does not convert existing members. See
[A-Series Sample Formats And Generations](sample-formats.md) for domains and
hardware distinctions, and [Writer And Alteration](write.md) for specification fields.

`update_sbnk_parameters` applies a partial
[`SampleParameters`](sample-parameters.md) object to one existing Sample.
It retains that Sample's stored 188/224-byte format. Later-only settings on a
native-format Sample are rejected rather than triggering conversion.
Fields omitted from the update and unrelated opaque bytes are preserved.
Dependent values are validated against the existing object, not fresh-object
defaults. For example, a key limit of `=Orig` uses that Sample's current root
key when the update omits `root_key`. Derived caches are recomputed from changed
source values. Sample Bank
`parameter_overrides` uses this model too, prepares every member update before
mutation, and leaves the bank's pending-propagation bits clear.

The update can additionally specify `playback_window` with unsigned
`start_frame` and positive `length_frames`. At least one parameter or a playback
window is required. Window edits require an ordinary current mono/stereo Sample
and complete, matching PCM8/PCM16 Wave Data. The resulting playback and loop
bounds are validated together, including retained loop values. The operation
updates active channel bounds and their end cache, without changing PCM or an
inactive channel's bytes. Optional `expected_payload_sha256` is the lowercase
SHA-256 of the complete source Sample payload; a mismatch rejects the update.

`convert_sbnk_format` explicitly converts an existing Sample between
`a3000_188` and `a4000_a5000_224`. It requires a lowercase SHA-256 of the
complete source payload and rejects stale inputs. For example, one manifest
operation is:

```json
{
  "id": "convert-sample",
  "type": "convert_sbnk_format",
  "partition_index": 0,
  "volume_name": "Strings",
  "sample_name": "Violin",
  "target_format": "a4000_a5000_224",
  "expected_payload_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

Replace the example digest with the current payload's digest. Conversion
preserves the Sample's identity, name, relationships and Wave Data, and commits
atomically. The planner reports affected fields and rejects unknown formats,
unsupported values, and changes that would lose parameter information. It does
not silently clamp values or discard active settings. Both directions are
subject to these checks; see [Sample Parameters](sample-parameters.md).
The resulting format identifies storage, not hardware-tested media compatibility.

`duplicate_sbnk` creates a standalone Sample in the source volume, pointing to
the same Wave Data. It requires `sample_name`, `new_name`, and `parameters`,
which may be empty. Optional parameter and playback-window edits use the update
contract above and affect only the copy. The source Sample, Sample Banks,
Programs, and Wave Data remain unchanged. The copied Sample's bank-membership
flag and Program-assignment bitmap are cleared; other unmodified payload bytes
are preserved. Supported sources are complete ordinary current mono/stereo
Samples with resolvable PCM8/PCM16 Wave Data and matching stereo rates/windows.
`expected_payload_sha256` guards the source payload. Case-insensitive collisions
with Sample names or existing Sample Bank/Program target names reject the
transaction, including unresolved references that would otherwise attach to
the new Sample. Allocation, validation, and insertion are atomic.

`update_program_parameters` applies [Program-wide and guarded assignment
parameter patches](program-parameters.md) to a current-layout Program. It
requires an explicit A4000/A5000 model and at least one writable leaf. Assignment
patches use a counted ordinal plus the expected stored target kind and name.
It preserves object size, unused rows, opaque state, and all other objects;
global and assignment edits are committed together or not at all.

`replace_program_assignments` supplies the complete ordered active row list
(zero through 999 rows). It requires `volume_name`, `program_number`, an explicit
`model`, and `expected_payload_sha256`, the lowercase SHA-256 of the complete
source Program payload at that point in the transaction. Each row is one of:

- `{"retain_ordinal": 2}` to preserve an existing row exactly;
- `{"retain_ordinal": 2, "sample": "New", "parameters": {"level_offset": 5}}`
  to retarget and optionally patch an existing row;
- `{"sample_bank": "Bank"}` or `{"sample": "Sample"}` to append a fresh row
  with optional assignment `parameters`.

Ordinals are zero-based and cannot be retained twice. Omitted active rows are
removed. Retained rows keep opaque state; fresh rows use neutral defaults.
Retargeting clears the old transient handle. The operation maintains target
Program bitmaps, preserves existing unused capacity and the complete parameter
tail, and grows allocation only when necessary. Stale payload identity,
unresolved targets, inconsistent bitmaps, or allocation failure reject the
whole transaction. Legacy Program conversion is not implicit.

`retarget_sample_wave_data` requires `volume_name`, `sample_name`, a new
`waveform_name`, and `expected_payload_sha256` for the complete source Sample.
Stereo Samples also require `right_waveform_name`. Both sources must exist in
the same volume. This operation preserves the Sample's mono/stereo source
topology, playback and loop windows, and parameters, while updating names,
references, rates, and derived pitch caches. Preserved windows must fit each
complete PCM8/PCM16 source; stereo source rates must agree. Different physical
lengths are allowed when both contain the preserved window. Duplicate-source
expanded mono and implicit topology conversion are not supported. All other
Sample bytes and all Wave Data bytes remain unchanged.

## Object deletion planning


Interactive clients review a deletion plan before deleting a Program, Sample
Bank, Sample, or Wave Data object. The plan accepts exact
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

## C++ SDK

Use the SDK's image-bound transaction interface for inspected changes to an
open image. See [C++ API](cpp-api.md) for preparation, application, progress,
and cancellation. The CLI alteration manifest above is a separate public
input contract; internal engine types are not an installed SDK interface.

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
