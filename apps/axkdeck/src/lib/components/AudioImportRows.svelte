<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import type { AudioImportAuditionState } from '../audio/audioImportAudition';
    import type { AudioImportCapabilities, AudioSourceInfo } from '../transport';
    import AudioImportDetails from './AudioImportDetails.svelte';
    import type { AudioImportRow, AudioImportValidation } from './audioImportDialogTypes';
    import AudioSamplerSettings from './AudioSamplerSettings.svelte';
    import Icon from './Icon.svelte';
    import SelectControl from './SelectControl.svelte';

    interface Props {
        rows: AudioImportRow[];
        validationErrors: AudioImportValidation[];
        capabilities?: AudioImportCapabilities;
        busy: boolean;
        committing: boolean;
        completionStatus?: string;
        audition: AudioImportAuditionState;
        onchangeTargetSampleRate: (row: AudioImportRow, rate: number) => void;
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
        completionStatus = '',
        audition,
        onchangeTargetSampleRate,
        onupdate,
        onremove,
        onaudition,
    }: Props = $props();
    let openDetailsId = $state<number>();
    const id = $props.id();

    function fitMessage(inspection: AudioSourceInfo): string {
        const perChannel = formatStoredSize(inspection.projectedOutputBytesPerChannel);
        return inspection.channels === 1
            ? `Fits · ${perChannel}`
            : `Fits · ${perChannel}/ch · ${formatStoredSize(inspection.projectedOutputBytesTotal)}`;
    }
    function conversionDescription(inspection: AudioSourceInfo): string {
        if (!inspection.sampleWidthConverted && !inspection.resampled) return `${inspection.outputSampleWidthBits}-bit`;
        if (!inspection.sampleWidthConverted)
            return `${inspection.outputSampleWidthBits}-bit · resampled ${inspection.quantized ? 'TPDF' : 'exactly'}`;
        const width = `${inspection.sourceSampleWidthBits} → ${inspection.outputSampleWidthBits}-bit`;
        return `${width} ${inspection.quantized ? 'TPDF' : 'exact'}`;
    }
    function updateWaveformName(row: AudioImportRow, index: number, event: Event): void {
        const waveformNames = [...row.waveformNames];
        waveformNames[index] = (event.currentTarget as HTMLInputElement).value;
        onupdate(row.id, { waveformNames });
    }
</script>

<div class="audio-import-rows">
    <div class="audio-import-column-headings" aria-hidden="true">
        <span>Source / conversion</span><span>Sample name</span><span>Wave Data (mono/left)</span>
        <span>Wave Data (right)</span><span>Target rate</span><span>Output</span>
    </div>
    {#each rows as row, index (row.id)}
        {@const validation = validationErrors[index]}
        {@const validationError = validation.message}
        {@const errorId = `${id}-error-${row.id}`}
        {@const invalidLeft =
            validation.field === 'waveformNames' &&
            (validation.waveformIndex === undefined || validation.waveformIndex === 0)}
        {@const invalidRight =
            validation.field === 'waveformNames' &&
            (validation.waveformIndex === undefined || validation.waveformIndex === 1)}
        {@const editable =
            row.status === 'ready' &&
            row.inspection !== undefined &&
            row.inspection.valid &&
            row.waveformNames.length === row.inspection.channels}
        {@const detailsOpen = openDetailsId === row.id}
        {@const detailsPanelId = `audio-import-details-${row.id}`}
        {@const hasAdjustments = row.inspection?.issues.some((issue) => issue.fatal === false) ?? false}
        {@const auditionActive = audition.rowId === row.id && ['preparing', 'playing'].includes(audition.status)}
        <section
            class="audio-import-card"
            class:editable={editable && !!capabilities}
            class:invalid={!!validationError}
            role="group"
            aria-label={`Audio import file ${row.fileName}`}
        >
            <div class="audio-import-file">
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
                <strong title={row.fileName}>{row.fileName}</strong>
            </div>
            <small class="audio-import-file-metadata">
                {#if row.status === 'uploading'}
                    Uploading {Math.round(row.progress * 100)}%
                {:else if row.status === 'failed'}
                    <span class="error-text">Upload or inspection failed</span>
                {:else if row.inspection}
                    <span
                        >{row.inspection.sourceFormat}
                        {row.inspection.sourceSubtype} · {row.inspection.channels === 2 ? 'Stereo' : 'Mono'}</span
                    >
                    <span class="metadata-separator"> · </span><span
                        >{row.inspection.sourceSampleRate.toLocaleString()} Hz · {conversionDescription(row.inspection)} ·
                        {row.inspection.durationSeconds.toFixed(2)} s</span
                    >
                {:else}Waiting{/if}
            </small>

            {#if editable && capabilities}
                <label class="sample-name">
                    <span>Sample name</span>
                    <input
                        class="dialog-field-control"
                        aria-label={`Sample name for ${row.fileName}`}
                        aria-invalid={validation.field === 'sampleName' || undefined}
                        aria-describedby={validation.field === 'sampleName' ? errorId : undefined}
                        data-dialog-initial-focus={index === 0 ? 'select' : undefined}
                        value={row.sampleName}
                        maxlength="16"
                        disabled={busy}
                        oninput={(event) =>
                            onupdate(row.id, { sampleName: (event.currentTarget as HTMLInputElement).value })}
                    />
                </label>
                <label class="wave-left">
                    <span>Wave Data (mono/left)</span>
                    <input
                        class="dialog-field-control"
                        aria-label={`Wave data (mono/left) for ${row.fileName}`}
                        aria-invalid={invalidLeft || undefined}
                        aria-describedby={invalidLeft ? errorId : undefined}
                        value={row.waveformNames[0]}
                        maxlength="16"
                        disabled={busy}
                        oninput={(event) => updateWaveformName(row, 0, event)}
                    />
                </label>
                {#if row.waveformNames.length === 2}
                    <label class="wave-right">
                        <span>Wave Data (right)</span>
                        <input
                            class="dialog-field-control"
                            aria-label={`Wave data (right) for ${row.fileName}`}
                            aria-invalid={invalidRight || undefined}
                            aria-describedby={invalidRight ? errorId : undefined}
                            value={row.waveformNames[1]}
                            maxlength="16"
                            disabled={busy}
                            oninput={(event) => updateWaveformName(row, 1, event)}
                        />
                    </label>
                {/if}
                <label class="target-rate">
                    <span>Target rate</span>
                    <SelectControl
                        label={`Target sample rate for ${row.fileName}`}
                        value={row.targetSampleRate}
                        options={capabilities.supportedSampleRates.map((rate) => ({
                            value: rate,
                            label: `${rate.toLocaleString()} Hz`,
                        }))}
                        disabled={busy || row.status === 'checking'}
                        onchange={(rate) => onchangeTargetSampleRate(row, rate)}
                    />
                </label>
                <AudioSamplerSettings {row} disabled={busy} {onupdate} invalidField={validation.field} {errorId} />
            {/if}

            <div class="card-header-actions">
                {#if completionStatus}
                    <span class="status-neutral">{completionStatus}</span>
                {:else if committing}
                    <span class="status-neutral">Importing…</span>
                {:else if row.status === 'waiting' || row.status === 'checking'}
                    <span class="status-neutral">Checking…</span>
                {:else if row.status === 'uploading'}
                    <span class="status-neutral">Uploading {Math.round(row.progress * 100)}%</span>
                {:else if row.status === 'inspected'}
                    <span class="status-neutral">Prepared</span>
                {:else if row.status === 'removing'}
                    <span class="status-neutral">Removing…</span>
                {:else if validationError}
                    <span class="status-message status-error"
                        ><Icon name="close" size={14} /><span
                            >{validation.conflict ? 'Name conflict' : 'Needs attention'}</span
                        ></span
                    >
                {:else if row.inspection}
                    <span class="status-message status-valid" title={fitMessage(row.inspection)}
                        ><Icon name="check" size={14} /><span>{fitMessage(row.inspection)}</span></span
                    >
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
                        <Icon name="info" size={14} />
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
                        onclick={() => onremove(row)}><Icon name="trash" size={14} /></button
                    >
                {/if}
            </div>
            {#if !committing && audition.rowId === row.id && audition.status === 'failed'}
                <p class="row-feedback error-text">Preview unavailable: {audition.error}</p>
            {/if}
            {#if validationError}<p id={errorId} class="row-feedback error-text" role="alert">{validationError}</p>{/if}
            {#if !committing && detailsOpen && row.inspection}
                <div class="row-details"><AudioImportDetails {row} panelId={detailsPanelId} /></div>
            {/if}
        </section>
    {/each}
</div>

<style>
    .audio-import-rows {
        container: audio-import-rows / inline-size;
        display: grid;
        align-content: start;
        min-height: 0;
        gap: 4px;
        overflow-y: auto;
        scroll-padding-block: 26px 4px;
        padding-right: 0;
        scrollbar-gutter: stable;
    }
    :global(.modal-overlay-scrollbars) .audio-import-rows {
        padding-right: 12px;
    }
    .audio-import-column-headings {
        display: none;
    }
    .audio-import-card {
        --audio-import-row-columns: minmax(259px, 1fr) 194px 184px 151px;
        --audio-import-parameter-columns: var(--audio-import-row-columns);
        display: grid;
        grid-template-columns: var(--audio-import-row-columns);
        align-items: start;
        gap: 4px;
        min-width: 0;
        padding: 6px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: var(--color-panel);
        color: var(--color-text);
        font-size: var(--dialog-body-font-size);
    }
    .audio-import-card.invalid {
        border-color: var(--color-danger);
    }
    .audio-import-file {
        display: flex;
        grid-column: 1 / 3;
        grid-row: 1;
        align-items: center;
        gap: 4px;
        min-width: 0;
        min-height: 26px;
    }
    .audio-import-file strong {
        min-width: 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: var(--dialog-section-font-size);
        font-weight: 600;
        line-height: 15px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .audition-button,
    .details-button,
    .row-remove-button {
        width: 26px;
        height: 26px;
        flex: none;
    }
    .audition-button,
    .details-button {
        color: var(--color-text-muted);
    }
    .audition-button:hover,
    .audition-button[aria-pressed='true'],
    .details-button:hover,
    .details-button[aria-expanded='true'] {
        color: var(--color-text-strong);
    }
    .audio-import-file-metadata {
        grid-column: 1 / -1;
        grid-row: 2;
        min-width: 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        line-height: 14px;
        overflow-wrap: anywhere;
    }
    label {
        display: grid;
        gap: 2px;
        min-width: 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
        line-height: 14px;
    }
    label > span {
        white-space: nowrap;
    }
    input {
        width: 100%;
        min-width: 0;
    }
    input[aria-invalid='true'] {
        border-color: var(--color-danger);
    }
    .sample-name {
        grid-column: 1;
        grid-row: 3;
    }
    .wave-left {
        grid-column: 2;
        grid-row: 3;
    }
    .wave-right {
        grid-column: 3;
        grid-row: 3;
    }
    .target-rate {
        grid-column: 4;
        grid-row: 3;
    }
    .card-header-actions {
        display: flex;
        grid-column: 3 / -1;
        grid-row: 1;
        align-items: center;
        justify-content: flex-end;
        gap: 4px;
        min-width: 0;
        min-height: 26px;
    }
    .status-message {
        display: flex;
        align-items: center;
        gap: 4px;
        min-width: 0;
        font-size: var(--dialog-metadata-font-size);
        line-height: 14px;
        overflow-wrap: anywhere;
    }
    .status-message :global(svg) {
        flex: none;
    }
    .status-valid {
        color: var(--color-success);
    }
    .status-error,
    .error-text {
        color: var(--color-danger);
    }
    .status-neutral {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        overflow-wrap: anywhere;
    }
    .details-button {
        position: relative;
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
        color: var(--color-danger);
    }
    .row-feedback,
    .row-details {
        grid-column: 1 / -1;
        min-width: 0;
        margin: 0;
        overflow-wrap: anywhere;
    }
    .row-feedback {
        font-size: var(--dialog-metadata-font-size);
        line-height: 14px;
    }
    @container audio-import-rows (min-width: 1230px) {
        .audio-import-card {
            --audio-import-row-columns: minmax(216px, 252px) 279px 194px 214px minmax(285px, 1fr);
            --audio-import-parameter-columns: 279px 194px 214px minmax(285px, 1fr);
            align-items: center;
        }
        .audio-import-column-headings {
            display: grid;
            position: sticky;
            top: 0;
            z-index: 1;
            grid-template-columns: minmax(216px, 252px) 279px 194px 214px 108px minmax(173px, 1fr);
            gap: 4px;
            padding: 4px 7px;
            background: var(--color-panel);
            color: var(--color-text-muted);
            font-size: var(--dialog-label-font-size);
            line-height: 14px;
        }
        .audio-import-file {
            grid-column: 1;
        }
        .audio-import-file-metadata {
            display: grid;
            grid-column: 1;
            grid-row: 2;
            min-height: 28px;
        }
        .metadata-separator {
            display: none;
        }
        .sample-name {
            grid-column: 2;
            grid-row: 1;
        }
        .wave-left {
            grid-column: 3;
            grid-row: 1;
        }
        .wave-right {
            grid-column: 4;
            grid-row: 1;
        }
        .target-rate {
            grid-column: 5;
            grid-row: 1;
            width: 108px;
        }
        .editable label > span {
            position: absolute;
            width: 1px;
            height: 1px;
            overflow: hidden;
            clip-path: inset(50%);
            white-space: nowrap;
        }
        .card-header-actions {
            grid-column: 5;
            grid-row: 1;
            width: calc(100% - 112px);
            justify-self: end;
        }
    }
    @container audio-import-rows (max-width: 813px) {
        .audio-import-card {
            --audio-import-row-columns: repeat(2, minmax(0, 1fr));
        }
        .audio-import-file,
        .audio-import-file-metadata {
            grid-column: 1 / -1;
        }
        .card-header-actions {
            grid-column: 1 / -1;
            grid-row: auto;
            justify-content: flex-start;
        }
        .sample-name,
        .wave-left,
        .wave-right,
        .target-rate {
            grid-row: auto;
        }
        .sample-name,
        .wave-right {
            grid-column: 1;
        }
        .wave-left,
        .target-rate {
            grid-column: 2;
        }
        label > span {
            white-space: normal;
        }
    }
    @container audio-import-rows (max-width: 465px) {
        .audio-import-card {
            --audio-import-row-columns: minmax(0, 1fr);
        }
        .sample-name,
        .wave-left,
        .wave-right,
        .target-rate {
            grid-column: 1;
        }
        .card-header-actions {
            flex-wrap: wrap;
        }
    }
</style>
