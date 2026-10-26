<script lang="ts">
    import type { AudioImportGrouping, AudioImportOptions } from '../audioImportOptions';
    import SampleFormatControl from './SampleFormatControl.svelte';
    import SelectControl from './SelectControl.svelte';

    let {
        mode = $bindable(),
        sampleBankName = $bindable(),
        sampleFormat = $bindable(),
        error,
        disabled,
    }: {
        mode: AudioImportGrouping['kind'];
        sampleBankName: string;
        sampleFormat: AudioImportOptions['sampleFormat'];
        error: string;
        disabled: boolean;
    } = $props();
</script>

<div class="import-target-settings">
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
    <span id="audio-import-format-label">Sample format</span>
    <SampleFormatControl bind:value={sampleFormat} label="Sample format" {disabled} />
    {#if mode === 'SAMPLE_BANK'}
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
    {/if}
</div>

<style>
    .import-target-settings {
        display: grid;
        grid-template-columns: max-content minmax(0, 360px) max-content max-content;
        align-items: center;
        gap: 6px 8px;
    }
    .import-target-settings > label,
    .import-target-settings > span {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
        white-space: nowrap;
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
