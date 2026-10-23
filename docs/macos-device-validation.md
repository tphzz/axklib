# Remote macOS Device Check

Build through **macOS CI**, selecting the source branch in **Run workflow**.
Leave **Debug** off for Release artifacts. Notarization is optional and off by
default; a signed build without notarization is not a Gatekeeper qualification.
Download the universal DMG from that run and install it on the test Mac.

Record macOS version, Intel or Apple Silicon hardware, application source
identity, reader model and whether notarization was requested. CI tests both
slices and the packaged server's device capabilities, but does not test a
physical USB device. Intel execution under Rosetta is not physical Intel
hardware acceptance.

## Read-Only Checks

1. Disconnect the sampler and any other SCSI host from the reader. Start
   axkdeck as a normal user, not as root.
2. In **Open > Device**, refresh the device list and select the intended USB
   device. Device access must be available in the packaged local backend.
3. Open read-only and approve the system authorization prompt when requested.
   Check the expected image contents, then close the device.
4. Repeat with authorization cancelled. The cancellation must be reported and
   a subsequent explicit open must remain possible.
5. Unplug and reconnect the reader while leaving axkdeck running. Refresh,
   select the new connection and open it again. An old selection must not
   silently open a replacement device.

## Write Qualification Is Separate

Do not write to an original card during these checks. Physical write testing
requires an explicitly chosen spare card, a verified backup and exclusive use
of the reader. Mounted media or uncertain ownership must refuse write access;
the application does not automatically unmount macOS volumes.

Record authorization, competing-use refusal, save/reopen and readback outcomes
separately when a physical write test is explicitly approved. A successful CI
run, capability check or read-only open does not qualify physical writes or
interrupted-write recovery. Include the source identity and relevant job/request
IDs with any failure report.
