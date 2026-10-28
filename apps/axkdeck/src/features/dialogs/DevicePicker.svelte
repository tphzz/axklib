<script lang="ts">
    import type { DeviceCandidate } from '../../lib/httpDevices';
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    let {
        id,
        candidates,
        value,
        disabled = false,
        refreshDisabled = false,
        onchange,
        onrefresh,
    }: {
        id: string;
        candidates: DeviceCandidate[];
        value: string;
        disabled?: boolean;
        refreshDisabled?: boolean;
        onchange: (value: string) => void;
        onrefresh: () => void;
    } = $props();
</script>

<div class="device-field">
    <label for={id}>Device</label>
    <div class="device-control">
        <SelectControl
            {id}
            label="Device"
            {value}
            {disabled}
            options={[
                { value: '', label: 'Select a device' },
                ...candidates.map((candidate) => ({
                    value: candidate.id,
                    label: `${candidate.model} / ${candidate.serial || 'No storage serial'} / ${(candidate.sizeBytes / 1e9).toFixed(2)} GB / ${candidate.sectorSize} B`,
                })),
            ]}
            emptyLabel="No devices available"
            {onchange}
        />
        <button
            class="icon-button"
            type="button"
            aria-label="Refresh devices"
            title="Refresh devices"
            disabled={disabled || refreshDisabled}
            onclick={onrefresh}><Icon name="refresh" size={14} /></button
        >
    </div>
</div>

<style>
    .device-field {
        display: grid;
        grid-template-columns: 110px minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        font-size: var(--dialog-label-font-size);
    }
    .device-control {
        display: flex;
        align-items: center;
        min-width: 0;
        gap: 8px;
    }
    .device-control > :global(.select-control) {
        flex: 1;
        min-width: 0;
        width: 100%;
    }
    @media (max-width: 520px) {
        .device-field {
            grid-template-columns: minmax(0, 1fr);
        }
    }
</style>
