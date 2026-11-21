# Retained Directional Conversion

Conversion uses a common prepare, review and execute lifecycle. It does not
translate every sampler through a universal intermediate model. Each registered
direction owns its mapping, fidelity policy and supported destination profile.

| Profile | Select | Receive |
| --- | --- | --- |
| `yamaha.tx16w-to-a-series` | A TX16W source and an existing SFS volume | Reviewed Wave Data or supported hierarchy in the A-series destination |
| `nuedge.typhoon-to-a-series` | A native Typhoon sound disk or explicit disk set | Wave Data, or Programs and Sample Banks built from selected Performances and Voices in an existing SFS image |
| `yamaha.tx16w-to-a-series.floppy`, `.iso`, `.sfs` | Yamaha disk set and volume name | A new A-series 1,440 KiB floppy, ISO CD-ROM or partitioned SFS image |
| `nuedge.typhoon-to-a-series.floppy`, `.iso`, `.sfs` | Typhoon disk set and volume name | The corresponding new A-series image, using the Typhoon instrument mapper |
| `yamaha.a-series-to-tx16w.wave-disk` | Ordered Wave Data occurrences in one native A-series document | A new 720 KiB FAT12 disk of mono TX16W Waves |
| `yamaha.a-series-to-tx16w.wave-disk-set` | The same ordered Wave Data selection | One or more numbered 720 KiB FAT12 images, each containing whole Waves |

The Wave disk contains no Setup, playable Voice or Performance. No
profile edits an existing TX16W disk or writes to a physical floppy.
The Yamaha Wave-disk exporter does not author Typhoon disks.

In **axkdeck**, open an A-series image and choose **Image options (...) > Convert...**
for the whole image, or **Convert...** from a volume's context menu for just that
volume. Select the Waves to export, choose a new server file or a download, review
the output, and choose **Create disk image**. This screen writes a single Yamaha
Wave disk. Larger Wave disk sets and fresh A-series media are available through
the converter API and file clients, not as additional destinations in this screen.

The built-in profile list is the same for the file SDK and server. Each
descriptor (including its options schema and `outputKind`) comes from the registration used
to prepare that direction. Registering a native reader does not implicitly
offer a conversion. Direct converters can map native instruments without an
intermediate format. For Wave Data, a registered `WaveReaderAdapter` and
`WaveWriterAdapter` automatically contribute a direction: the reader resolves
the source selection and supplies bounded mono PCM and playback metadata; the
writer maps those Waves into its destination. Registering either adapter alone
does not produce a route. Selection controls belong to the reader and media
controls to the writer.

A profile's `route` is either null (no interactive editor is offered) or
describes its target label, medium, intents, required editor capabilities,
and priority. Match the source, destination, output kind and intent before
considering priority. An editor must support all required panels. Lower
priority numbers are preferred; a dialog without a route chooser must not
silently break a tie. Profile IDs identify registrations, not GUI components.

For a fresh A-series destination, append the full suffix to the source's
profile ID, for example `nuedge.typhoon-to-a-series.iso`. Supply `volumeName`
alongside the usual `mode` and optional `sourceObjectIds`. The SFS route also
requires a sector-aligned `sizeBytes`, `partitionCount` (1 to 8) and zero-based
`partitionIndex`. For example, `8388608`, `2`, `1` places the volume in the
second partition of a new 8 MiB disk. The requested image must fit the preparation
memory limit; a valid SFS geometry is not a promise that an 8 GiB image can be
materialized in the current process. No destination file is needed for preparation.

## Review And Execution

Preparation is read-only. An `EXISTING_IMAGE` profile inventories the target
and retains patches against its fingerprint. A `NEW_IMAGE` profile retains
complete image bytes instead; review reports their size and SHA-256.
`NEW_IMAGE_SET` retains a bounded, ordered collection of complete images.
None reruns mapping, resampling or encoding at execution.

The report identifies planned objects and native source occurrences, together
with destination names, slots, relationships, key ranges and waveform settings.
Mapping issues have these impacts:

- `EXACT`: directly representable semantics.
- `REPRESENTATION`: encoding or numbering changes without meaningful loss.
- `LOSS`: meaningful approximation or omission.
- `UNCERTAINTY`: unproven behavior or an explicit default.
- `BLOCKER`: conversion cannot safely proceed.

`BEST` reports losses and uncertainties as conversion notes. No acknowledgement
is required unless an issue includes a `decision` prompt: for example, creating
an audition Program from a Voice when there is no source Performance.
`STRICT` blocks loss and uncertainty instead. Neither mode bypasses missing/ambiguous references,
destination validation or sampler capacity admission. Equal source names or
payload bytes do not merge occurrences. Wave Data-only conversion is an explicit
TX16W option, not an automatic fallback for a broken hierarchy.

Plans expire after 15 minutes and are not persisted across server restarts.
The server admits at most 128 plans, 512 MiB per plan and 1 GiB across retained
payloads and pending preparation reservations. Opening a private source set
charges its decoded catalog during preparation. An already open document
reuses the workspace catalog; preparing one Wave does not reserve another
copy of the entire library. The plan retains its report, output, and source
leases for identity and revision checks, not the decoded catalog. Idle expiry releases
leases; no workspace access mutex remains locked while a user reviews a plan.

An accepted plan has one execution job. Repeated execution requests return that
job even with a different transport idempotency key. Failure and cancellation
never make a consumed plan reusable. Terminal plans release their buffers and
retain the accepted job identity for a further 15 minutes. A changed source,
destination or workspace revision requires a new plan.

For an existing destination, server execution uses the same guarded, journaled session commit as ordinary
mutations, including rollback, interrupted-write recovery and revision
publication. For a new image it publishes an owner-scoped download of the
raw image, or writes the exact reviewed bytes to the writable workspace
`file` selected in the prepare request. Neither path replaces a file, mutates
the source or increments its revision. Conversion
itself never owns device authorization or recovery.

## Generic HTTP Resources

All resources use the server's usual authorization and response envelopes.
Native identities include both `imageId` and `documentId`; the source and target
may be different documents in the same workspace.

| Request | Purpose |
| --- | --- |
| `GET /api/v1/conversion-profiles` | Available directions, options schemas and interactive route descriptors |
| `POST /api/v1/conversion-inspections` | Read a native source catalog and applicable profile IDs without an output or retained plan |
| `POST /api/v1/conversion-plans` | Submit a read-only preparation job |
| `GET /api/v1/conversion-plans/{planId}?offset=0&limit=200` | Owner-scoped plan and report page |
| `DELETE /api/v1/conversion-plans/{planId}` | Release an unsubmitted plan |
| `POST /api/v1/conversion-plans/{planId}/executions` | Submit or retrieve its one execution job |

Inspect with `source`, optional native `objectType`, zero-based `offset` and
`limit` (1 to 200). An explicit source set also requires `sourceFormat`.
The completed inspection job returns `documentId`, `sourceFormat`,
`profileIds`, and a page of native occurrences in `items` with the filtered
`total`. Keep the source and revision fixed while paging. This does not
prepare a conversion or reserve a destination.

Preparation uses `profileId`, `source`, `destination`, `options`, `fidelity` and
optional `capacityPolicy`. `source` is a revision-bound `DOCUMENT`, or a `SOURCES`
array of explicit `FILE`, `DIRECTORY` or admitted `UPLOAD` references. Physical
devices must first be opened as owned workspaces. For `EXISTING_IMAGE`,
`destination` contains the target document, `expectedRevision` and the
profile's `selection`. For the TX16W-to-A-series direction,
selection is `{ "kind": "EXISTING_VOLUME", "partitionIndex": 0, "volumeName": "Samples" }`.
Use `CREATE_VOLUME` to create and populate a named volume in the selected
partition in one transaction. A name collision is an error, not a merge. Options
include `"mode": "HIERARCHY"` or `"mode": "WAVE_DATA_ONLY"`. Omit
`options.sourceObjectIds` to plan the whole set. To plan a subset, supply a
nonempty array of unique IDs from selectable rows in the source catalog for
that mode. The profile includes required dependencies automatically.

For `NEW_IMAGE`, pass `"destination": {"kind": "NEW_IMAGE"}`, with no
destination session, selection or capacity policy. The completed execution
job contains `download.contentPath`; GET it as the same principal to receive
the bytes, not an archive wrapper. To publish on the server instead, include
the destination `file` (a writable-root FileRef) at preparation. An existing
output is never replaced. A completed job with `file` and a
`publication_durability_unconfirmed` warning means the file was published
but the directory sync failed. Report the warning; do not retry the write.
For the TX16W Wave disk use the public
document ID and Wave IDs from the source session in `options.documentId`
and `options.waveIds`. Use the same document and its current revision in
`source`. File-client IDs are not substitutes for session IDs.

For a `NEW_IMAGE_SET` route, pass `"destination": {"kind": "NEW_IMAGE_SET"}`.
Review returns an ordered `destination.members` array: each entry has
`filename`, `sizeBytes` and `sha256`. Execution publishes **one** owner-scoped
TAR download, not independent downloads. Its root contains the reviewed
images and `manifest.json` with the same ordered `members` and `profileId`.
Read disk order from the manifest, not the archive entry or directory order.

The preparation job result contains the plan ID and first report page. Fetch
remaining pages using `actionCount`, `issueCount` and, when a selectable source
catalog is available, `sourceCount`. Each collection is sliced at the same
offset. Rows in `sourceObjects` include the native occurrence ID, name, kind,
location, eligibility (`selectable`) and direct `dependencies` by ID.
Use these IDs from the same source binding, not names, for selection.
For execution, pass `{"acknowledgedIssueIds":[]}` when the plan has no decisions.
Otherwise pass exactly the IDs of confirmed `decision` prompts; note IDs,
duplicates and unknown IDs are rejected. Execution also requires an
`Idempotency-Key`. Keep the returned job identity until completion is confirmed.
After a lost submission response, retrieve the plan's `jobId`, then its status;
do not submit a newly prepared replacement write blindly.

The old sampler-specific TX16W inspection endpoint and
`import_tx16w_disk_set` alteration operation are removed, not aliases.

## Installed SDK And CLI

`<axklib/sdk/conversion.hpp>` is a C++17 installed facade. Discover directions
with `conversion_profiles_json()`. `conversion_plan::prepare(request, context)`
returns a move-only, in-process plan. Inspect `report_json()`, which includes
the full source catalog when supplied by the profile. Call
`apply(new_output_path, issue_ids, context)` with the confirmed decision IDs
or an empty list when no mapping choice is needed.
The result reports publication durability and any post-publication warnings.

For TX16W input the file facade accepts explicit FAT images or source folders
and an existing destination image template. Snapshots are bounded to 64 MiB
and 4096 files. For A-series input, a regular SFS image is read by range and
fingerprinted, not held as an image-sized snapshot. SFS input is bounded to
8 GiB in aggregate. The file facade rejects source folder symlinks,
nonregular entries, excessive depth and changed source membership. Publication
copies and fingerprints the destination, applies retained patches, and atomically
creates a new output. For a new-image profile leave both destination fields
empty; the file review's `newImage` describes the retained bytes. Both forms
publish only to a new path, never overwriting an existing file or input.
An accepted execution attempt consumes the plan even on failure or cancellation.

The CLI uses the same in-process file plan:

```bash
axklib convert --profile yamaha.tx16w-to-a-series \
  --source tx16w-files --destination template.hds \
  --selection '{"kind":"EXISTING_VOLUME","partitionIndex":0,"volumeName":"Samples"}' \
  --options '{"mode":"HIERARCHY"}' --output converted.hds
```

The command prints the prepared report and requests `APPLY` before publication.
If the report contains explicit mapping decisions, append their IDs as shown
in the prompt. Ordinary losses do not change the confirmation. Without `--output`, it
only prepares and prints the report. `--strict` disallows loss and uncertainty.
No serialized report can later be submitted as an executable plan.

## Import a Typhoon sound disk

You can inspect the disks without converting them. Pass the members together.
On mixed disks, select the document with `formatId: "nuedge.typhoon"` for
Typhoon data; the Yamaha document has its own object identities.

```bash
axklib info --source-set keys-a.img keys-b.img
axklib convert --profile nuedge.typhoon-to-a-series \
  --source keys-a.img keys-b.img --destination template.hds \
  --selection '{"kind":"CREATE_VOLUME","partitionIndex":0,"volumeName":"Keys"}' \
  --options '{"mode":"HIERARCHY"}'
```

The example is a dry run. Append `--output imported.hds` to publish a new
copy after reviewing the plan. Use `EXISTING_VOLUME` to add to a named
volume instead. `WAVE_DATA_ONLY` transfers audio independently of instrument
dependencies. In `HIERARCHY`, a Performance becomes a Program, a Voice a
Sample Bank, and each playable split a Sample pointing to Wave Data. A
shared source Wave is stored once. A Voice selected on its own does not
acquire a fabricated Performance.

Review lists key and velocity ranges, Fixed Pitch, Sample Coarse and Fine Tune,
and the assigned Wave.
Group and split ranges are reported in MIDI notes: native keys 0 through
108 correspond to MIDI 12 through 120 before Performance transpose. Empty
splits stay silent, and the projection does not shift the Wave root.
For an unlooped Wave, a Normal Group uses forward playback (`-->`), which
honors note-off; a One-shot Group uses `One ->`, which plays to the end.
A sustain loop is kept. A looped One-shot, Glide or Release trigger cannot
be fully reproduced; Best reports the approximation and Strict blocks it.
In particular, a Release Group is not triggered on key-up on the target.

Group octave/semitone and fine settings tune each split's Sample, not the
shared Wave. A Group coarse setting of +12 raises playback one octave.
Both fine controls use 1,024 steps per octave: with neutral Wave tuning,
Group raw +32 becomes Sample Fine +32 (about +37.5 cents). When the Wave
has a pitch origin, its detune is subtracted from the Group fine setting.
The converter uses the available coarse and fine ranges together; a pitch
outside their combined range is clipped in Best and reported as a loss.

Fixed group scaling additionally turns on Fixed Pitch and adds the
difference between MIDI 60 and the Wave's Typhoon pitch origin.
For roots 48, 60 and 72 the correction is +12, 0 and -12 semitones.
Typhoon limits valid origins to MIDI 12 through 120. An absent AIFF
`INST` chunk or equal low/high notes means no origin and no correction.
The stored Wave root and audio are unchanged. Normal scaling leaves Fixed
Pitch off and adds no correction. Other scaling values fall back to normal
tracking with a loss in Best and block Strict conversion. Envelopes,
filters, modulation and Performance controls are not fully
translated. Best reports omissions; Strict blocks them. A Voice with an
explicitly empty Wave map is different from a missing Wave: Best can omit
the former, never substitute for the latter. Select an unaffected instrument
or supply the required disk and version before retrying.

## Create a TX16W Wave disk

First inspect the source and choose the Wave by its volume and occurrence,
not by its display name. Insert the returned IDs in the following commands:

```bash
axklib info --source-set source.hds
axklib info --source-set source.hds --document "$doc" --collection objects
options=$(jq -nc --arg doc "$doc" --arg wave "$wave" '{documentId:$doc,waveIds:[$wave]}')
axklib convert --profile yamaha.a-series-to-tx16w.wave-disk --source source.hds --options "$options"
axklib convert --profile yamaha.a-series-to-tx16w.wave-disk --source source.hds --options "$options" --output waves.img
axklib info --source-set waves.img
```

Use `SMPL` (Wave Data) IDs, not Sample or Sample Bank IDs. Selection order
assigns slots `.W01` through `.W64`, so two Waves with the same name remain
distinct. The base name is uppercased and shortened to eight ASCII characters;
unsupported characters become underscores. Review shows both names.

A forward one-shot maps to 33,333 Hz. A forward loop uses the nearer of
33,333 and 50,000 Hz and must end at the playback boundary. Attack and repeat
must each fit 64 through 131,071 frames. A one-shot still uses two native
parts, without looping. Pitch metadata is reported as a loss: a Wave-only
disk does not store the source root key or fine tune. On conversion, signed
audio is rounded to the nearest 12-bit level (ties away from zero) and
saturated at the endpoints. Review reports rate changes, loop rounding and
precision loss. A STRICT plan with loss cannot execute.

File sizes include 512-byte padding; allocation uses 1,024-byte clusters.
For `wave-disk`, all selected Waves must fit one disk. For
`wave-disk-set`, the writer starts the next disk when the next whole Wave will
not fit. Selection order is preserved: for example, if the first two Waves
fit on `DISK01.IMG`, the third retains its `.W03` slot on `DISK02.IMG`.
Review identifies each Wave's disk. The 64-slot limit applies to the whole set,
not per disk. Neither route splits a Wave, writes instrument assignments, or
publishes a partial set when a Wave cannot be represented.

For an ordered set, use the same selection with a **new directory** as
the SDK or CLI output. Preparation alone creates nothing:

```bash
axklib convert --profile yamaha.a-series-to-tx16w.wave-disk-set --source source.hds --options "$options" --output disks
```

The file review lists the disk names, lengths and SHA-256 hashes in
`newImageSet.members`. Publication creates the directory atomically after
review, and refuses to replace even an empty directory.

## Axkdeck

From the TX16W Device view, choose **Convert**, browse the source server for
an existing writable A-series SFS image, then select the partition and volume.
The conversion adds objects to that volume; it does not replace the image with
a TX16W disk. This profile does not create a new image or format a physical
disk. The source workspace stays selected; the temporary destination session
is closed on clean completion or cancellation.

Dropping TX16W disks onto an open A-series destination uses the same review
policy. Select source occurrences and inspect the resulting objects and their
dependencies. Known changes such as resampling appear as conversion notes;
they do not require checkboxes. Changing inputs releases the obsolete plan
and clears mapping decisions. After a confirmed write, refresh failure offers
**Refresh**, never another write. An
uncertain submission offers **Check status** using the retained plan/job identity.
