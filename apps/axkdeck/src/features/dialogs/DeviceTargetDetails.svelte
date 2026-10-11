<script lang="ts">
    import type { DeviceSession } from '../../lib/httpDevices';
    import { deviceTargetType } from './deviceTargetPresentation';
    let { target }: { target: DeviceSession['targets'][number] | undefined } = $props();
</script>

<dl class="device-target-details" aria-label="Configured target details">
    <div>
        <dt>Type</dt>
        <dd>{target ? deviceTargetType(target) : 'No target selected'}</dd>
    </div>
    <div>
        <dt>Sector size</dt>
        <dd>{target ? `${target.sectorSize} B` : '-'}</dd>
    </div>
    <div>
        <dt>Start (bytes)</dt>
        <dd>{target?.offsetBytes ?? '-'}</dd>
    </div>
    <div>
        <dt>End (exclusive, bytes)</dt>
        <dd>{target ? target.offsetBytes + target.sizeBytes : '-'}</dd>
    </div>
    <div>
        <dt>Vendor / product</dt>
        <dd>{target?.scsi ? `${target.scsi.vendor} ${target.scsi.product}`.trim() || '-' : '-'}</dd>
    </div>
    <div>
        <dt>Revision / serial</dt>
        <dd>{target?.scsi ? `${target.scsi.revision} / ${target.scsi.serial}` : '-'}</dd>
    </div>
</dl>

<style>
    .device-target-details {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
        gap: 6px 16px;
        margin: 0;
    }
    .device-target-details div {
        min-width: 0;
    }
    dt {
        font-size: var(--dialog-label-font-size);
        color: var(--color-text-muted);
    }
    dd {
        margin: 2px 0 0;
        font-size: var(--dialog-body-font-size);
        overflow-wrap: anywhere;
    }
</style>
