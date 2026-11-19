<script lang="ts">
    import DiskImportSettings from './DiskImportSettings.svelte';
    import SelectControl from './SelectControl.svelte';
    import ConversionRouteHeader from './ConversionRouteHeader.svelte';
    let {
        source,
        sourceLabel,
        profile,
        targetLabel,
        destination,
        disabled,
        onbrowse,
    }: {
        source?: string;
        sourceLabel: string;
        profile?: string;
        targetLabel?: string;
        destination?: string;
        disabled: boolean;
        onbrowse?: () => void;
    } = $props();
</script>

<ConversionRouteHeader {source} {sourceLabel} {profile} {targetLabel} {disabled} />
<DiskImportSettings>
    <div class="destination-row">
        <strong>Destination</strong>
        <SelectControl
            label="Destination medium"
            class="medium-select"
            value="image"
            options={[{ value: 'image', label: 'Image file' }]}
            {disabled}
            onchange={() => undefined}
        />
        <span>Image</span>
        <div
            class="dialog-segmented-control dialog-segmented-control--compact"
            role="group"
            aria-label="Destination image type"
        >
            <button aria-pressed="true" {disabled}>Existing</button>
            <button aria-pressed="false" disabled title="This conversion writes to an existing image">New</button>
        </div>
        <div class="file-choice">
            <label for="conversion-file">File</label><input
                id="conversion-file"
                class="dialog-field-control"
                readonly
                value={destination ?? ''}
                placeholder="Choose an existing image"
            />
            <button class="secondary-button" disabled={disabled || !onbrowse} onclick={onbrowse}>Browse...</button>
        </div>
    </div>
</DiskImportSettings>

<style>
    .destination-row {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 8px;
        min-width: 0;
        font-size: var(--dialog-body-font-size);
    }
    .destination-row :global(.medium-select) {
        flex: 0 1 145px;
        width: 145px;
        max-width: 100%;
    }
    .destination-row {
        width: 100%;
    }
    .destination-row strong {
        min-width: 100px;
    }
    span,
    label {
        color: var(--color-text-muted);
    }
    input {
        min-width: 100px;
        flex: 1;
    }
    .file-choice {
        display: flex;
        align-items: center;
        gap: 8px;
        flex: 1 1 240px;
        min-width: 0;
    }
    @container disk-import (max-width:680px) {
        .destination-row strong {
            min-width: 0;
        }
        .file-choice {
            flex-basis: 100%;
        }
    }
</style>
