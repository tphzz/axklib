<script lang="ts">
    import type { DeviceCandidate } from '../../lib/httpDevices';
    let { candidate }: { candidate: DeviceCandidate } = $props();
</script>

<dl class="identity" aria-label="Device identity">
    <dt>Storage model / serial</dt>
    <dd>{candidate.model || 'Not reported'} / {candidate.serial || 'Not reported'}</dd>
    <dt>USB reader</dt>
    <dd>
        {candidate.identity.usbManufacturer || 'Manufacturer not reported'} / {candidate.identity.usbProduct ||
            'Product not reported'}
    </dd>
    <dt>USB VID:PID / serial</dt>
    <dd>
        {candidate.identity.usbVendorId || '?'}:{candidate.identity.usbProductId || '?'} / {candidate.identity
            .usbSerial || 'Not reported'}
    </dd>
    <dt>Native device</dt>
    <dd>{candidate.identity.devicePath || 'Not reported'}</dd>
    <dt>Carrier capacity</dt>
    <dd>
        {candidate.sizeBytes.toLocaleString()} bytes / {(candidate.sizeBytes / 1e9).toFixed(2)} GB / {candidate.sectorSize}
        B sectors
    </dd>
</dl>

<style>
    .identity {
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
        min-width: 0;
        overflow-wrap: anywhere;
        font-size: var(--dialog-body-font-size);
    }
    @media (max-width: 450px) {
        .identity {
            grid-template-columns: 106px minmax(0, 1fr);
        }
    }
</style>
