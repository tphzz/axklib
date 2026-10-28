<script lang="ts">
    import type { DeviceInspection } from '../../lib/httpDeviceFormatting';
    import DeviceIdentity from '../dialogs/DeviceIdentity.svelte';
    let { inspection, partitions }: { inspection: DeviceInspection; partitions: number } = $props();
</script>

<DeviceIdentity candidate={inspection.candidate} />
<dl class="scope" aria-label="Formatting scope">
    <dt>Target</dt>
    <dd>{inspection.target.label}</dd>
    <dt>Target range (bytes)</dt>
    <dd>
        {inspection.target.offsetBytes.toLocaleString()} to {(
            inspection.target.offsetBytes + inspection.target.sizeBytes
        ).toLocaleString()} (end exclusive)
    </dd>
    <dt>New filesystem</dt>
    <dd>
        Yamaha SFS / {partitions}
        {partitions === 1 ? 'partition' : 'partitions'} / {inspection.formattedSizeBytes.toLocaleString()} bytes
    </dd>
    <dt>Unused target suffix</dt>
    <dd>{inspection.unusedSuffixBytes.toLocaleString()} bytes</dd>
</dl>
<p class="dialog-warning">
    All existing data on this target will be lost. This is a quick format, not a secure erase. Bytes outside the new
    filesystem are not a preserved or usable old filesystem.
</p>
<p class="reader-note">
    USB descriptors may identify the reader or bridge, not the inserted card. Verify the physical device and card.
</p>

<style>
    .scope {
        display: grid;
        grid-template-columns: 130px minmax(0, 1fr);
        gap: 4px 8px;
        margin: 0;
    }
    dt {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }
    dd {
        margin: 0;
        overflow-wrap: anywhere;
        font-size: var(--dialog-body-font-size);
    }
    p {
        margin: 0;
        font-size: var(--dialog-body-font-size);
    }
    .reader-note {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    @media (max-width: 450px) {
        .scope {
            grid-template-columns: 106px minmax(0, 1fr);
        }
    }
</style>
