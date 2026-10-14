# Retained Directional Conversion

Conversion uses a common prepare, review and execute lifecycle. It does not
translate every sampler through a universal intermediate model. Each registered
direction owns its mapping, fidelity policy and supported destination profile.
The initial direction is `yamaha.tx16w-to-a-series`, into an existing SFS volume.
TX16W native editing and writing are separate, unsupported capabilities.

## Review And Execution

Preparation is read-only. It resolves the native source graph, inventories the
destination, translates parameters, materializes audio, allocates objects and
retains the resulting byte patches. Execution applies those exact patches; it
does not rerun mapping or resampling after review.

The report identifies planned objects and native source occurrences, together
with destination names, slots, relationships, key ranges and waveform settings.
Mapping issues have these impacts:

- `EXACT`: directly representable semantics.
- `REPRESENTATION`: encoding or numbering changes without meaningful loss.
- `LOSS`: meaningful approximation or omission.
- `UNCERTAINTY`: unproven behavior or an explicit default.
- `BLOCKER`: conversion cannot safely proceed.

`BEST` allows loss and uncertainty only after each issue ID is acknowledged.
`STRICT` blocks them. Neither mode bypasses missing/ambiguous references,
destination validation or sampler capacity admission. Equal source names or
payload bytes do not merge occurrences. Wave Data-only conversion is an explicit
TX16W option, not an automatic fallback for a broken hierarchy.

Plans expire after 15 minutes and are not persisted across server restarts.
The server admits at most 128 plans, 512 MiB per plan and 1 GiB across retained
payloads and pending preparation reservations. Source metadata and leases are
charged conservatively as well as report and patch buffers. Idle expiry releases
leases; no workspace access mutex remains locked while a user reviews a plan.

An accepted plan has one execution job. Repeated execution requests return that
job even with a different transport idempotency key. Failure and cancellation
never make a consumed plan reusable. Terminal plans release their buffers and
retain the accepted job identity for a further 15 minutes. A changed source,
destination or workspace revision requires a new plan.

Server execution uses the same guarded, journaled session commit as ordinary
mutations, including rollback, interrupted-write recovery and revision
publication. Conversion itself never owns device authorization or recovery.

## Generic HTTP Resources

All resources use the server's usual authorization and response envelopes.
Native identities include both `imageId` and `documentId`; the source and target
may be different documents in the same workspace.

| Request | Purpose |
| --- | --- |
| `GET /api/v1/conversion-profiles` | Available directions and options schemas |
| `POST /api/v1/conversion-plans` | Submit a read-only preparation job |
| `GET /api/v1/conversion-plans/{planId}?offset=0&limit=200` | Owner-scoped plan and report page |
| `DELETE /api/v1/conversion-plans/{planId}` | Release an unsubmitted plan |
| `POST /api/v1/conversion-plans/{planId}/executions` | Submit or retrieve its one execution job |

Preparation uses `profileId`, `source`, `destination`, `options`, `fidelity` and
optional `capacityPolicy`. `source` is a revision-bound `DOCUMENT`, or a `SOURCES`
array of explicit `FILE`, `DIRECTORY` or admitted `UPLOAD` references. Physical
devices must first be opened as owned workspaces. `destination` contains the
target document, `expectedRevision` and the profile's `selection`. For TX16W,
selection is `{ "partitionIndex": 0, "volumeName": "Samples" }` and options
are `{ "mode": "HIERARCHY" }` or `{ "mode": "WAVE_DATA_ONLY" }`.

The preparation job result contains the plan ID and first report page. Fetch
remaining pages using `actionCount` and `issueCount`; each collection is sliced
at the same offset. Execution accepts only `acknowledgedIssueIds` and requires an
`Idempotency-Key`. Keep the returned job identity until completion is confirmed.
After a lost submission response, retrieve the plan's `jobId`, then its status;
do not submit a newly prepared replacement write blindly.

The old sampler-specific TX16W inspection endpoint and
`import_tx16w_disk_set` alteration operation are removed, not aliases.

## Installed SDK And CLI

`<axklib/sdk/conversion.hpp>` is a C++17 installed facade. Discover directions
with `conversion_profiles_json()`. `conversion_plan::prepare(request, context)`
returns a move-only, in-process plan. Inspect `report_json()`, obtain explicit
review acknowledgements, then call `apply(new_output_path, issue_ids, context)`.
The result reports publication durability and any post-publication warnings.

The file facade accepts explicit FAT images or source folders and an existing
destination image template. Source snapshots are bounded to 64 MiB and 4096
files, within the plan's 512 MiB budget. It rejects source folder symlinks,
nonregular entries, excessive depth and changed source membership. Publication
copies and fingerprints the destination, applies retained patches, and atomically
creates a new output. It never overwrites an existing file, including an input.
An accepted execution attempt consumes the plan even on failure or cancellation.

The CLI uses the same in-process file plan:

```bash
axklib convert --profile yamaha.tx16w-to-a-series \
  --source tx16w-files --destination template.hds \
  --selection '{"partitionIndex":0,"volumeName":"Samples"}' \
  --options '{"mode":"HIERARCHY"}' --output converted.hds
```

The command prints the prepared report and requests an exact confirmation
including every required issue ID before publication. Without `--output`, it
only prepares and prints the report. `--strict` disallows loss and uncertainty.
No serialized report can later be submitted as an executable plan.

## Axkdeck

Destination-first TX16W import still prepares automatically after source, target,
mode, fidelity or capacity changes. Source-first **Convert** in the TX16W Device
view browses the source server for a destination image, opens a temporary target
session and uses the same review dialog. The source workspace stays selected;
the temporary target is closed on clean completion or cancellation.

Changing inputs releases the obsolete plan and clears acknowledgements. After a
confirmed write, refresh failure offers **Refresh**, never another write. An
uncertain submission offers **Check status** using the retained plan/job identity.
