<script lang="ts">
    import DiskImportSettings from './DiskImportSettings.svelte';
    import SelectControl from './SelectControl.svelte';
    import AttributeHelp from './AttributeHelp.svelte';
    let {
        source,
        sourceLabel,
        profile,
        destination,
        disabled,
        onbrowse,
    }: {
        source?: string;
        sourceLabel: string;
        profile?: string;
        destination?: string;
        disabled: boolean;
        onbrowse?: () => void;
    } = $props();
</script>

<div class="route">
    <span>Source</span><strong>{sourceLabel}</strong><span class="path"
        >{#if source}<AttributeHelp label={source} description={source} />{/if}</span
    >
    <div class="target-format">
        <strong>Target format</strong>
        <SelectControl
            label="Target format"
            class="format-select"
            value={profile}
            options={profile ? [{ value: profile, label: 'A-series · Hard disk (SFS)' }] : []}
            disabled={disabled || !profile}
            onchange={() => undefined}
        />
    </div>
</div>
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
    .route,
    .destination-row {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 8px;
        min-width: 0;
        font-size: var(--dialog-body-font-size);
    }
    .route {
        flex: none;
        min-height: 32px;
    }
    .target-format {
        display: flex;
        align-items: center;
        gap: 8px;
        max-width: 100%;
    }
    .target-format strong {
        white-space: nowrap;
    }
    .route :global(.format-select) {
        flex: 0 1 230px;
        width: 230px;
        max-width: 100%;
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
    .path {
        flex: 1;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
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
        .path {
            flex-basis: 45%;
        }
        .destination-row strong {
            min-width: 0;
        }
        .file-choice {
            flex-basis: 100%;
        }
    }
</style>
