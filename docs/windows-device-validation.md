# Windows Device Checks

Use the installer from **10 Build - Windows x64** or **11 Build - Windows ARM64**,
matching the test computer. These workflows produce test artifacts, not a
published release.
Record the application's source identity from its startup log before testing.

These checks open devices read-only. Do not use Save, Import, Delete, restore or
any other command that writes to the card. Disconnect the sampler and other
SCSI hosts before attaching the USB reader to the computer.

## Capture Slow Sample Selection

This trace measures the existing behavior; it does not yet change read sizes,
prefetch, waveform quality or safety checks. Use **10 Build - Windows x64** for
the first capture and record the workflow run URL and application's source
identity. Keep the same card, reader and connection for all three runs.

1. Start the application, open **Logs**, and set **Level** to **Debug**. Keep
   the Logs window open so detailed capture remains active. Confirm its footer
   also shows the local server at Debug; report an error there before testing.
2. Open the device read-only and turn **Autoplay** off. Open the Sample waveform
   editor. Choose a Sample A that is slow and has not already been selected or
   restored automatically in this app launch. Choose a different Sample B too.
3. Select **A**, wait for its waveform, select **B**, wait again, then select
   **A** again. Do not click other Samples or Audition during these three waits.
   Note the Sample names, approximate waits, and whether A's second visit is fast.
4. After the last waveform is visible, optionally press **Audition** once.
   In Logs, use **Save logs > Save all logs...**, not just the filtered view.
5. Quit the application completely. Repeat steps 1-4 twice more, saving each
   run separately. Restarting clears application caches; this is not a claim
   that Windows or the reader's hardware cache was cleared.

Return the three log files, Windows version, reader model, Sample names, and
workflow/source identity. An existing matching backup image on NVMe can be a
separate control; do not create a card dump or change device permissions for this
check. Autoplay off deliberately leaves today's speculative audio prefetch intact.

### Reading The Capture

`sample_read_frontend` joins selection, preview queue, HTTP, editor cache,
audio-prefetch and waveform-draw records. `selectionId` identifies the click;
`sinceSelectionMs` on `waveform-first-draw` is the time to the first nonempty
editor canvas draw, not a compositor/display-presentation timestamp. Later
`waveform-refinement-draw` records identify higher-resolution draws. A completion
with `current: false` still belongs to its original selection, not the new one.

Join HTTP `requestId` to `image_object_read`, `image_preview_read`,
`image_audition_prepare_read`, `image_audition_range_read` and
`image_read_response`. Audition submission also records `jobId`; prepared audio
and range records share `auditionId`. Backend phase durations and `startOffsetUs`
separate lock waiting, source verification, preparation and PCM processing.
`serialize` measures response construction separately.

For each slow click, summarize selection-to-draw time, queue/lock waits,
verification time, PCM time, read count/bytes and cache results. Use these rules:

- Phase durations partition a backend call; `total` is not another phase to add.
- Enumeration and layout-guard times are inside source verification. PCM read
  time includes transport time. Frontend HTTP/job waits contain backend work.
  These nested timings must not be added together.
- `transportReads`/`transportReadBytes` count attempted post-cache transport
  calls, not necessarily individual Windows disk operations or completed bytes.
  `failedReads` identifies failures. Layout-guard reads are counted separately.
  Device-only counters are absent for ordinary image files; their PCM timings
  still apply.
- The read-size buckets are disjoint: up to 4 KiB, over 4 through 64 KiB, over
  64 KiB through 1 MiB, and over 1 MiB. Cache hits and misses count metadata pages.
- `helperReads`/`directReads` identify the actual native access path, including
  layout guards. Transport time includes helper IPC where used; it is not an
  isolated measurement of physical disk time.

Linux tests and synthetic read counts verify the trace, not Windows performance.
Choose the optimization only after the Windows capture identifies the delay.

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
