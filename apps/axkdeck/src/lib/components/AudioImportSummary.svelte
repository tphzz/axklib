<script lang="ts">
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import { formatStoredSize } from '../formatBytes';
    import type { AudioImportRow } from './audioImportDialogTypes';
    import AttributeHelp from './AttributeHelp.svelte';
    import Icon from './Icon.svelte';
    import ImportCapacitySummary from './ImportCapacitySummary.svelte';

    let { rows, capacity }: { rows: AudioImportRow[]; capacity: ImportCapacityReview } = $props();
    const prepared = $derived(rows.filter((row) => row.status === 'ready' && row.inspection?.valid));
    const waves = $derived(prepared.reduce((count, row) => count + row.waveformNames.length, 0));
    const bytes = $derived(
        prepared.reduce((count, row) => count + (row.inspection?.projectedOutputBytesTotal ?? 0), 0),
    );
    const rates = $derived([...new Set(prepared.map((row) => row.inspection?.outputSampleRate))]);
    const output = $derived(
        prepared.length
            ? `${rates.length === 1 ? `${rates[0]?.toLocaleString()} Hz` : 'Mixed rates'} · 16-bit output`
            : '',
    );
</script>

<div class="audio-import-summary">
    {#if rows.length}
        <span class="source-counts">
            {rows.length}
            {rows.length === 1 ? 'file' : 'files'} ·
            {#if prepared.length === rows.length}
                {prepared.length}
                {prepared.length === 1 ? 'Sample' : 'Samples'} · {waves} Wave Data · {formatStoredSize(bytes)} output
            {:else}
                {prepared.length} prepared
            {/if}
        </span>
    {/if}
    <ImportCapacitySummary review={capacity} compact />
    {#if output}
        <span class="output-format">
            <AttributeHelp
                label="Audio output details"
                description={`${output}. ${prepared.length} Samples; ${waves} Wave Data; ${formatStoredSize(bytes)} output.`}
            >
                <Icon name="info" size={14} />
            </AttributeHelp>
            {output}
        </span>
    {/if}
</div>

<style>
    .audio-import-summary {
        display: flex;
        flex: none;
        align-items: center;
        flex-wrap: wrap;
        gap: 4px;
        min-width: 0;
        min-height: 26px;
    }
    .source-counts {
        color: var(--color-text);
        font-size: var(--dialog-body-font-size);
    }
    .audio-import-summary > :global(.capacity-summary) {
        flex: 1 1 300px;
    }
    .output-format {
        display: flex;
        align-items: center;
        gap: 8px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        white-space: nowrap;
    }
</style>
