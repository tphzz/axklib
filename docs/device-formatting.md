# Device initialization safety contract

## Scope

Create has Image, Device, SCSI2SDv6 and SCSI2SD XML destinations. Image remains
the default and retains the existing HD and FAT12 floppy profiles. Physical
initialization creates an empty Yamaha SFS prefix of `min(target bytes, 8 GiB)`;
partition choices come from the same planner as image creation, using the
actual target capacity. Unused suffix bytes and sibling targets are not a
preserved old filesystem. This is a quick format, not secure erasure.

No physical-device acceptance is established by the synthetic tests. Existing
Windows ARM64 and Linux ARM64 write restrictions remain in force.

## Decision And Rationale

Formatting is a separate capability from editing a decoded image. Open and
Create share tabs, device controls, target descriptions and the native provider;
Create never borrows an ordinary editing write handle. Inspection and Review
use a fresh read-only connection. Read permission may still require OS approval.
Write access is requested only after explicit destructive consent.

The native policy is `ReadWriteUnmounted`, carried unchanged through the helper
protocol. It must refuse mounted or otherwise busy storage without dismounting
anything. Current native claims cover one OS disk, so multi-LUN USB carriers
are refused even when their individual mount inventories are available.

On macOS, the retained Disk Arbitration claim and mount-approval callback are
used. Linux checks whole-disk and partition holders, visible mount namespaces
and active swap, and also requires a read-only exclusive kernel claim during
inspection. Process enumeration alone cannot establish unmounted state: hidden
namespaces and multi-device filesystems can omit physical device numbers from
the visible mount list. An unavailable read-only claim or inaccessible metadata
makes inspection incomplete and blocks formatting. After consent, the writable
exclusive claim is retained and its opened physical identity is rechecked. See
the [kernel block-device claim contract](https://origin.kernel.org/doc/html/v6.8/core-api/kernel-api.html#c.bd_prepare_to_claim).
Windows formatting is currently unavailable: neither a complete non-dismounting
mount inventory nor a retained automatic-remount guard has been qualified.
An exclusive PhysicalDrive handle alone does not establish that protection. Ordinary
device editing is unchanged. See Microsoft's
[volume-lock requirements](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume).

Automatic dismount, a mounted-volume override, ordinary-write authorization
reuse and filesystem-signature-only permission were rejected because they
weaken the destructive operation's independent safety boundary.

## Inspection And Consent

Native storage model/serial and USB manufacturer/product/VID:PID/serial are
displayed separately from the native device path and capacity. USB descriptors
may identify a reader or bridge rather than its inserted card. Missing fields
are explicit; descriptive fields are not used as substitutes for native binding.

Raw inspection reads at most 8 MiB, accepts at most 128 partition entries and
cannot return a successful result after its ten-second deadline. Synchronous
native I/O cannot be forcibly interrupted at that deadline. Header-only SFS,
ordinary MBR and primary/backup GPT structures are checked; common filesystem
headers are labelled as signatures rather than full filesystem validation.
Unknown contents are never described as blank. Malformed, ambiguous or
unsupported partition structures block formatting. All native carrier members
are inspected, and any mounted member blocks formatting.

Partition and filesystem ranges use absolute carrier coordinates, including
partitions starting outside a selected SCSI slice and both GPT metadata copies.
Cross-target filesystems or shared parent partition metadata block that slice.
Inspection bytes and every planned overwritten sector are guarded against
changes through the final admission boundary.

Review issues an owner-bound, single-use plan, valid for fifteen minutes. Every
physical format requires the server-provided exact `ERASE ...` text and both
data-loss and exclusive-use acknowledgements. Cancel receives initial focus;
Enter in the text field does not submit the operation.

## Ownership, Journalling And Recovery

The client persists an operation identity before POST, without requiring an
image session. Backend admission consumes the plan and durably reserves the
carrier before publishing a queued job. Queued cancellation or rejection fences
NO_WRITE and releases ownership before publishing a terminal state. Retained
operations fence conflicting writes across owners and layout aliases.

Original bytes are durably journalled before any device write. A final one-shot
forward gate follows journal reads and guard verification immediately before
the first write attempt. Cancellation or expiry before that gate performs no
restoration writes. After a possible modification, rollback does not depend on
forward consent, cancellation or plan expiry. Lost binding/claim or failed
restoration retains RECOVERY_REQUIRED and the journal.

Commit requires exact planned-byte readback and strict SFS header, mirror,
geometry, allocation and empty-inventory checks. A job's terminal state alone
does not clear client format tracking: durable NO_WRITE, COMMITTED or ROLLED_BACK
must be retrieved. Uncertain submissions offer status reconciliation, never a
blind resubmission. A confirmed format with a failed reopen offers Open only,
reconnecting read-only when the old session was closed during cleanup.

Format recovery requires a fresh read-only connection, fresh exact `RESTORE ...`
text, both acknowledgements and the same non-dismounting native policy. The
journal records the original physical target bounds and logical operation
extent; recovery never derives writable bounds from damaged SFS bytes. It
checks untouched physical guards and recognizes original/replacement bytes
before restoration. Its first restoration write has a separate cancellation
gate; automatic rollback never uses that gate.

Valid smaller SFS prefixes reopen with a bounded logical reader and mutation
view on carriers above 8 GiB, including across process restarts. Damaged smaller
declarations cannot widen that writable view. Valid legacy full-extent SFS up to
9 GB retains its existing full view. Physical identity always uses full carrier
and target geometry, independently of the logical filesystem extent.

## Remaining Native Qualification

- Run all six native build workflows without weakening their write gates.
- Qualify packaged focus, cancellation, approval and retained-session cleanup
  on native WebView2 and WKWebView; Chromium is not native acceptance.
- Establish a non-dismounting Windows mount inventory and automatic-remount
  guard before enabling Windows formatting.
- Separately approve sacrificial media before any physical format or recovery
  acceptance. Check mounted siblings, late mounts, reconnects, rollback,
  recovery, unused suffixes and protected SCSI2SD configuration sectors.

No real disk is modified by the default regression campaign. The optional
rawdisk loop-device test must remain disabled for ordinary local validation.
