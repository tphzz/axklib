<script lang="ts">
    import type { AudioImportGrouping, AudioImportOptions } from '../audioImportOptions';
    import SampleFormatControl from './SampleFormatControl.svelte';

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
    <select id="audio-import-mode" class="dialog-field-control" bind:value={mode} {disabled}>
        <option value="SAMPLES">Import as Samples</option>
        <option value="SAMPLE_BANK">Import as Samples in a Sample Bank</option>
    </select>
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
    select,
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
