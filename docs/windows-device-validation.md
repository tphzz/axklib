# Windows Device Reconnection Check

Use the installer from **Windows x64 CI** or **Windows ARM64 CI**, matching the
test computer. These workflows produce test artifacts, not a published release.
Record the application's source identity from its startup log before testing.

These checks open devices read-only. Do not use Save, Import, Delete, restore or
any other command that writes to the card. Disconnect the sampler and other
SCSI hosts before attaching the USB reader to the computer.

## Reconnect Without Restarting

1. Start axkdeck. In **Open > Device**, refresh, select the intended USB device
   and open it read-only. Approve the administrator prompt when requested.
2. Close the open device, unplug the reader, reconnect it and wait for Windows
   to finish detecting it. Keep axkdeck running.
3. Refresh the device list, select the device again and open it read-only.
   Repeat this ten times. A newly selected connection must open without
   restarting axkdeck.
4. If using a removable card reader, repeat the sequence by changing only the
   card. Record this separately from unplugging the whole reader.

## Refuse An Old Selection

1. Refresh and select a device, but do not open it yet.
2. Unplug and reconnect the reader, then try to open the old selection without
   refreshing. It must be rejected, even when its name and capacity are unchanged.
3. Refresh, select the new connection and open it. This must succeed without
   restarting the application.
4. Repeat with the unplug/reconnect occurring while the administrator prompt
   is open. Approving that old request must not open the replacement connection.
5. Cancel an administrator prompt, then retry with a fresh selection. The
   cancelled attempt must not prevent the next valid open.

The helper protocol also has automated rejection-and-retry tests. If two USB
devices are available, keep the first open while opening the second to check
reuse of the existing elevated helper; record whether a new prompt appeared.

## Report The Result

Record Windows version, CPU architecture, application source identity, reader
model, whether the reader or only the card was changed, the number of repeats,
and whether administrator approval was requested. For any failure, include the
device job's request ID, job ID and error code from Logs.

Connection, descriptor, geometry and unavailable-identity failures now have
distinct error codes. The device must still be rejected when Windows cannot
provide a complete identity; a missing identity must never bypass validation.

A passing build or these read-only checks do not qualify physical writes or
interrupted-write recovery.
