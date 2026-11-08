<script lang="ts">
    import type { Snippet } from 'svelte';
    import type { AudioImportGrouping, AudioImportOptions } from '../audioImportOptions';
    import SampleFormatControl from './SampleFormatControl.svelte';
    import SelectControl from './SelectControl.svelte';

    let {
        mode = $bindable(),
        sampleBankName = $bindable(),
        sampleFormat = $bindable(),
        error,
        disabled,
        compact = false,
        showImportSettings = true,
        children,
    }: {
        mode: AudioImportGrouping['kind'];
        sampleBankName: string;
        sampleFormat: AudioImportOptions['sampleFormat'];
        error: string;
        disabled: boolean;
        compact?: boolean;
        showImportSettings?: boolean;
        children?: Snippet;
    } = $props();
</script>

{#snippet bankSettings()}
    <label for="audio-import-sample-bank-name">Sample Bank name</label>
    <input
        id="audio-import-sample-bank-name"
        class="dialog-field-control"
        aria-invalid={error !== ''}
        bind:value={sampleBankName}
        maxlength="16"
        autocomplete="off"
        {disabled}
    />
    {#if error}<p class="field-error" role="alert">{error}</p>{/if}
{/snippet}

<div class="import-target-settings" class:compact>
    {#if showImportSettings}
        <div class="import-mode-setting">
            <label for="audio-import-mode">Import mode</label>
            <SelectControl
                id="audio-import-mode"
                label="Import mode"
                value={mode}
                onchange={(value) => (mode = value)}
                {disabled}
                options={[
                    { value: 'SAMPLES', label: 'Import as Samples' },
                    { value: 'SAMPLE_BANK', label: 'Import as Samples in a Sample Bank' },
                ] as const}
            />
        </div>
        <div class="sample-format-setting">
            <span id="audio-import-format-label">Sample format</span>
            <SampleFormatControl bind:value={sampleFormat} label="Sample format" {disabled} {compact} />
        </div>
    {/if}
    {@render children?.()}
    {#if showImportSettings && !compact && mode === 'SAMPLE_BANK'}{@render bankSettings()}{/if}
</div>
{#if showImportSettings && compact && mode === 'SAMPLE_BANK'}
    <div class="import-bank-settings">{@render bankSettings()}</div>
{/if}

<style>
    .import-target-settings {
        display: grid;
        grid-template-columns: max-content minmax(0, 360px) max-content max-content;
        align-items: center;
        gap: 6px 8px;
    }
    label,
    .sample-format-setting > span {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
        white-space: nowrap;
    }
    .import-mode-setting,
    .sample-format-setting {
        display: contents;
    }
    .import-target-settings.compact {
        display: flex;
        flex: 1 1 716px;
        flex-wrap: wrap;
        gap: 4px;
        min-width: 0;
    }
    .compact .import-mode-setting,
    .compact .sample-format-setting {
        display: flex;
        align-items: center;
        gap: 4px;
        min-width: 0;
    }
    .compact .import-mode-setting > label {
        width: 68px;
        flex: none;
    }
    .compact .import-mode-setting > :global(.select-control) {
        width: var(--audio-import-mode-width, 280px);
    }
    .compact .sample-format-setting > span {
        width: 80px;
        flex: none;
    }
    .import-bank-settings {
        display: grid;
        flex-basis: 100%;
        grid-template-columns: 104px minmax(0, 220px);
        align-items: center;
        gap: 4px;
    }
    .import-bank-settings .field-error {
        grid-column: 2;
    }
    @container audio-import (max-width: 545px) {
        .compact .import-mode-setting {
            flex: 1 1 100%;
        }
        .compact .import-mode-setting > :global(.select-control) {
            width: auto;
            flex: 1;
        }
    }
    @container audio-import (max-width: 365px) {
        .compact .import-mode-setting {
            flex-direction: column;
            align-items: stretch;
        }
        .compact .sample-format-setting {
            flex-wrap: wrap;
        }
        .compact .import-mode-setting > :global(.select-control) {
            width: 100%;
            flex: none;
        }
        .import-bank-settings {
            grid-template-columns: minmax(0, 1fr);
        }
        .import-bank-settings .field-error {
            grid-column: 1;
        }
    }
    input {
        width: 100%;
        min-width: 0;
    }
    .field-error {
        grid-column: 3 / -1;
        margin: 0;
        color: var(--color-danger);
        font-size: var(--dialog-body-font-size);
    }
    @media (max-width: 760px) {
        .import-target-settings {
            grid-template-columns: max-content minmax(0, 1fr);
        }
        :global(.dialog-segmented-control) {
            justify-self: start;
        }
        .field-error {
            grid-column: 2;
        }
    }
</style>
