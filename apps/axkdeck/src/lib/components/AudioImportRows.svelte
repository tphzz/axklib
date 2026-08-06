<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import type { AudioImportAuditionState } from '../audio/audioImportAudition';
    import type { AudioImportCapabilities, AudioSourceInfo } from '../transport';
    import AudioImportDetails from './AudioImportDetails.svelte';
    import type { AudioImportRow } from './audioImportDialogTypes';
    import AudioSamplerSettings from './AudioSamplerSettings.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        rows: AudioImportRow[];
        validationErrors: string[];
        capabilities?: AudioImportCapabilities;
        busy: boolean;
        committing: boolean;
        grouped: boolean;
        audition: AudioImportAuditionState;
        onchangeTargetSampleRate: (row: AudioImportRow, event: Event) => void;
        onupdate: (id: number, update: Partial<AudioImportRow>) => void;
        onremove: (row: AudioImportRow) => void;
        onaudition: (row: AudioImportRow) => void;
    }

    let {
        rows,
        validationErrors,
        capabilities,
        busy,
        committing,
        grouped,
        audition,
        onchangeTargetSampleRate,
        onupdate,
        onremove,
        onaudition,
    }: Props = $props();
    let openDetailsId = $state<number>();

    function fitMessage(inspection: AudioSourceInfo): string {
        const perChannel = formatStoredSize(inspection.projectedOutputBytesPerChannel);
        if (inspection.channels === 1) return `Fits · ${perChannel}`;
        return `Fits · ${perChannel}/channel · ${formatStoredSize(inspection.projectedOutputBytesTotal)} total`;
    }

    function conversionDescription(inspection: AudioSourceInfo): string {
        if (!inspection.sampleWidthConverted && !inspection.resampled) return `${inspection.outputSampleWidthBits}-bit`;
        if (!inspection.sampleWidthConverted)
            return `${inspection.outputSampleWidthBits}-bit · resampled ${inspection.quantized ? 'TPDF' : 'exactly'}`;
        const width = `${inspection.sourceSampleWidthBits} → ${inspection.outputSampleWidthBits}-bit`;
        if (inspection.quantized) return `${width} TPDF`;
        return `${width} exact`;
    }

    function updateWaveformName(row: AudioImportRow, index: number, event: Event): void {
        const waveformNames = [...row.waveformNames];
        waveformNames[index] = (event.currentTarget as HTMLInputElement).value;
        onupdate(row.id, { waveformNames });
    }
</script>

<p class="audio-import-summary">
    Each file creates one Sample and {rows.some((row) => row.inspection?.channels === 2) ? 'mono or stereo' : 'mono'} Wave
    Data. {grouped ? 'The Samples will be added to the new Sample Bank.' : 'The Samples remain standalone.'}
    WAV sampler metadata is mapped when it can be represented safely.
</p>
<div class="audio-import-rows">
    {#each rows as row, index (row.id)}
        {@const validationError = validationErrors[index]}
        {@const editable =
            row.status === 'ready' &&
            row.inspection !== undefined &&
            row.inspection.valid &&
            row.waveformNames.length === row.inspection.channels}
        {@const detailsOpen = openDetailsId === row.id}
        {@const detailsPanelId = `audio-import-details-${row.id}`}
        {@const hasAdjustments = row.inspection?.issues.some((issue) => issue.fatal === false) ?? false}
        {@const auditionActive = audition.rowId === row.id && ['preparing', 'playing'].includes(audition.status)}
        <section class="audio-import-card" role="group" aria-label={`Audio import file ${row.fileName}`}>
            <header class="card-header">
                <div class="audio-import-file">
                    <div class="audio-import-file-heading">
                        <strong title={row.fileName}>{row.fileName}</strong>
                        {#if row.status === 'ready' && row.inspection?.valid}
                            <button
                                class="icon-button audition-button"
                                type="button"
                                aria-label={`${auditionActive ? 'Stop' : 'Play'} ${row.fileName}`}
                                aria-pressed={auditionActive}
                                title={auditionActive ? 'Stop preview' : 'Play preview'}
                                disabled={busy}
                                onclick={() => onaudition(row)}
                            >
                                <Icon name={auditionActive ? 'stop' : 'play'} size={14} />
                            </button>
                        {/if}
                    </div>
                    {#if row.status === 'uploading'}
                        <small>Uploading {Math.round(row.progress * 100)}%</small>
                    {:else if row.status === 'failed'}
                        <small class="error-text">Upload or inspection failed</small>
                    {:else if row.inspection}
                        <small>
                            {row.inspection.sourceFormat}
                            {row.inspection.sourceSubtype} ·
                            {row.inspection.channels === 2 ? 'Stereo' : 'Mono'} ·
                            {row.inspection.sourceSampleRate.toLocaleString()} Hz ·
                            {conversionDescription(row.inspection)}
                            · {row.inspection.durationSeconds.toFixed(2)} s
                        </small>
                    {:else}
                        <small>Waiting</small>
                    {/if}
                    {#if !committing && audition.rowId === row.id && audition.status === 'failed'}
                        <small class="error-text">Preview unavailable: {audition.error}</small>
                    {/if}
                </div>
                <div class="card-header-actions">
                    {#if committing}
                        <span class="status-neutral">Importing…</span>
                    {:else if row.status === 'waiting'}
                        <span class="status-neutral">Checking…</span>
                    {:else if row.status === 'uploading'}
                        <span class="status-neutral">Uploading {Math.round(row.progress * 100)}%</span>
                    {:else if row.status === 'checking'}
                        <span class="status-neutral">Checking…</span>
                    {:else if row.status === 'inspected'}
                        <span class="status-neutral">Prepared</span>
                    {:else if row.status === 'removing'}
                        <span class="status-neutral">Removing…</span>
                    {:else if validationError}
                        <span class="status-message status-error">
                            <Icon name="close" size={14} />
                            <span>{validationError}</span>
                        </span>
                    {:else if row.inspection}
                        <span class="status-message status-valid">
                            <Icon name="check" size={14} />
                            <span>{fitMessage(row.inspection)}</span>
                        </span>
                    {/if}
                    {#if !committing && row.status === 'ready' && row.inspection}
                        <button
                            class:has-adjustments={hasAdjustments}
                            class="details-button icon-button"
                            type="button"
                            aria-label={`Import details for ${row.fileName}`}
                            aria-expanded={detailsOpen}
                            aria-controls={detailsPanelId}
                            title="Import details"
                            onclick={() => (openDetailsId = detailsOpen ? undefined : row.id)}
                        >
                            <Icon name="info" size={15} />
                            {#if hasAdjustments}<span class="adjustment-marker" aria-hidden="true"></span>{/if}
                        </button>
                    {/if}
                    {#if !committing && ['ready', 'failed'].includes(row.status) && validationError}
                        <button
                            class="icon-button row-remove-button"
                            type="button"
                            aria-label={`Remove ${row.fileName}`}
                            title="Remove file"
                            disabled={busy}
                            onclick={() => onremove(row)}
                        >
                            <Icon name="trash" size={15} />
                        </button>
                    {/if}
                </div>
            </header>

            {#if editable && capabilities}
                <div class="identity-fields">
                    <label>
                        <span>Target rate</span>
                        <select
                            aria-label={`Target sample rate for ${row.fileName}`}
                            value={row.targetSampleRate}
                            disabled={busy || row.status === 'checking'}
                            onchange={(event) => onchangeTargetSampleRate(row, event)}
                        >
                            {#each capabilities.supportedSampleRates as rate (rate)}
                                <option value={rate}>{rate.toLocaleString()} Hz</option>
                            {/each}
                        </select>
                    </label>
                    <label>
                        <span>Sample name</span>
                        <input
                            aria-label={`Sample name for ${row.fileName}`}
                            data-dialog-initial-focus={index === 0 ? 'select' : undefined}
                            value={row.sampleName}
                            maxlength="16"
                            disabled={busy}
                            oninput={(event) =>
                                onupdate(row.id, {
                                    sampleName: (event.currentTarget as HTMLInputElement).value,
                                })}
                        />
                    </label>
                    <label>
                        <span>Wave Data (mono/left)</span>
                        <input
                            aria-label={`Wave data (mono/left) for ${row.fileName}`}
                            value={row.waveformNames[0]}
                            maxlength="16"
                            disabled={busy}
                            oninput={(event) => updateWaveformName(row, 0, event)}
                        />
                    </label>
                    {#if row.waveformNames.length === 2}
                        <label>
                            <span>Wave Data (right)</span>
                            <input
                                aria-label={`Wave data (right) for ${row.fileName}`}
                                value={row.waveformNames[1]}
                                maxlength="16"
                                disabled={busy}
                                oninput={(event) => updateWaveformName(row, 1, event)}
                            />
                        </label>
                    {/if}
                </div>
                <AudioSamplerSettings {row} disabled={busy} {onupdate} />
            {/if}

            {#if !committing && detailsOpen && row.inspection}
                <AudioImportDetails {row} panelId={detailsPanelId} />
            {/if}
        </section>
    {/each}
</div>

<style>
    .audio-import-summary {
        margin: 0;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .audio-import-rows {
        display: grid;
        min-height: 0;
        gap: 8px;
        overflow-y: auto;
        padding-right: 12px;
        scrollbar-gutter: stable;
    }
    .audio-import-card {
        display: grid;
        gap: 10px;
        min-width: 0;
        padding: 11px 12px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: rgb(255 255 255 / 1.5%);
        color: var(--color-text);
        font-size: 12px;
    }
    .card-header {
        display: grid;
        grid-template-columns: minmax(0, 1fr) auto;
        align-items: center;
        gap: 12px;
    }
    .audio-import-file {
        min-width: 0;
    }
    .audio-import-file-heading {
        display: flex;
        align-items: center;
        gap: 5px;
        min-width: 0;
    }
    .audio-import-file strong {
        display: block;
        min-width: 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 12px;
        font-weight: 600;
        line-height: 1.35;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .audition-button {
        width: 24px;
        height: 24px;
        flex: none;
        color: var(--color-text-muted);
    }
    .audition-button:hover,
    .audition-button[aria-pressed='true'] {
        color: var(--color-text-strong);
    }
    .audio-import-file small {
        display: block;
        overflow: hidden;
        margin-top: 3px;
        color: var(--color-text-muted);
        font-size: 11px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .card-header-actions {
        display: flex;
        align-items: center;
        justify-content: flex-end;
        gap: 6px;
        min-width: 0;
    }
    .identity-fields {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
        gap: 9px 12px;
    }
    label {
        display: grid;
        min-width: 0;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    input,
    select {
        width: 100%;
        min-width: 0;
        height: var(--density-control);
        padding: 0 8px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 5px;
        outline: none;
        background: var(--color-bg-deep);
        font-size: 12px;
    }
    select {
        appearance: auto;
    }
    input:focus,
    select:focus {
        border-color: var(--color-accent);
    }
    .status-message {
        display: flex;
        align-items: flex-start;
        gap: 6px;
        max-width: 420px;
        font-size: 11px;
        line-height: 1.35;
        overflow-wrap: anywhere;
    }
    .status-message :global(svg) {
        flex: none;
        margin-top: 1px;
    }
    .status-valid {
        color: var(--color-success);
    }
    .status-error,
    .error-text {
        color: var(--color-danger) !important;
    }
    .status-neutral {
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .details-button {
        position: relative;
        flex: none;
        color: var(--color-text-muted);
    }
    .details-button:hover,
    .details-button[aria-expanded='true'] {
        color: var(--color-text-strong);
    }
    .details-button.has-adjustments {
        color: var(--color-warning);
    }
    .adjustment-marker {
        position: absolute;
        top: 4px;
        right: 4px;
        width: 5px;
        height: 5px;
        border: 1px solid var(--color-panel);
        border-radius: 50%;
        background: currentColor;
    }
    .row-remove-button {
        flex: none;
        color: var(--color-danger);
    }
    @media (max-width: 900px) {
        .card-header {
            align-items: start;
        }
        .card-header-actions {
            align-items: flex-end;
            flex-direction: column;
        }
        .status-message {
            max-width: 280px;
            text-align: right;
        }
    }
    @media (max-width: 620px) {
        .card-header {
            grid-template-columns: minmax(0, 1fr);
        }
        .card-header-actions {
            align-items: center;
            justify-content: flex-start;
            flex-direction: row;
            flex-wrap: wrap;
        }
        .status-message {
            max-width: none;
            text-align: left;
        }
    }
</style>
