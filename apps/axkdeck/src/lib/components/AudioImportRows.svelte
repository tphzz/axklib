<script lang="ts">
    import { noteName } from '../audioImport';
    import { formatStoredSize } from '../formatBytes';
    import type { AudioImportCapabilities, AudioSourceInfo } from '../transport';
    import type { AudioImportRow } from './audioImportDialogTypes';
    import AudioSamplerSettings from './AudioSamplerSettings.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        rows: AudioImportRow[];
        validationErrors: string[];
        capabilities?: AudioImportCapabilities;
        busy: boolean;
        onchangeTargetSampleRate: (row: AudioImportRow, event: Event) => void;
        onupdate: (id: number, update: Partial<AudioImportRow>) => void;
        onremove: (row: AudioImportRow) => void;
    }

    let { rows, validationErrors, capabilities, busy, onchangeTargetSampleRate, onupdate, onremove }: Props = $props();

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
    Each file creates one standalone Sample and {rows.some((row) => row.inspection?.channels === 2)
        ? 'mono or stereo'
        : 'mono'} Wave Data. WAV sampler metadata is mapped when it can be represented safely.
</p>
<div class="audio-import-rows">
    <table class="audio-import-table">
        <colgroup>
            <col class="source-column" />
            <col class="rate-column" />
            <col class="sample-column" />
            <col class="wave-column" />
            <col class="wave-column" />
            <col class="root-key-column" />
            <col class="status-column" />
            <col class="action-column" />
        </colgroup>
        <thead>
            <tr>
                <th scope="col">Source file</th>
                <th scope="col">Target rate</th>
                <th scope="col">Sample name</th>
                <th scope="col">Wave data (mono/left)</th>
                <th scope="col">Wave data (right)</th>
                <th scope="col">Root key</th>
                <th scope="col">Status</th>
                <th scope="col"><span class="visually-hidden">Actions</span></th>
            </tr>
        </thead>
        <tbody>
            {#each rows as row, index (row.id)}
                {@const validationError = validationErrors[index]}
                {@const editable =
                    row.status === 'ready' &&
                    row.inspection !== undefined &&
                    row.inspection.valid &&
                    row.waveformNames.length === row.inspection.channels}
                <tr>
                    <td class="source-cell" data-label="Source file">
                        <div class="audio-import-file">
                            <strong title={row.fileName}>{row.fileName}</strong>
                            {#if row.status === 'uploading'}
                                <small>Uploading {Math.round(row.progress * 100)}%</small>
                            {:else if row.status === 'failed'}
                                <small class="error-text">{row.error}</small>
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
                        </div>
                    </td>
                    <td data-label="Target rate">
                        {#if row.inspection?.valid && capabilities}
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
                        {:else}
                            <span class="audio-import-unavailable" aria-hidden="true">—</span>
                        {/if}
                    </td>
                    <td data-label="Sample name">
                        {#if editable}
                            <input
                                aria-label="Sample name"
                                data-dialog-initial-focus={index === 0 ? 'select' : undefined}
                                value={row.sampleName}
                                maxlength="16"
                                oninput={(event) =>
                                    onupdate(row.id, {
                                        sampleName: (event.currentTarget as HTMLInputElement).value,
                                    })}
                            />
                        {:else}
                            <span class="audio-import-unavailable" aria-hidden="true">—</span>
                        {/if}
                    </td>
                    <td data-label="Wave data (mono/left)">
                        {#if editable}
                            <input
                                aria-label="Wave data (mono/left)"
                                value={row.waveformNames[0]}
                                maxlength="16"
                                oninput={(event) => updateWaveformName(row, 0, event)}
                            />
                        {:else}
                            <span class="audio-import-unavailable" aria-hidden="true">—</span>
                        {/if}
                    </td>
                    <td data-label="Wave data (right)">
                        {#if editable && row.waveformNames.length === 2}
                            <input
                                aria-label="Wave data (right)"
                                value={row.waveformNames[1]}
                                maxlength="16"
                                oninput={(event) => updateWaveformName(row, 1, event)}
                            />
                        {:else if editable}
                            <span class="audio-import-unavailable" aria-label="No right wave data">—</span>
                        {:else}
                            <span class="audio-import-unavailable" aria-hidden="true">—</span>
                        {/if}
                    </td>
                    <td data-label="Root key">
                        {#if editable}
                            <span class="root-key-control"
                                ><input
                                    value={row.rootKey}
                                    type="number"
                                    min="0"
                                    max="127"
                                    aria-label="Root key"
                                    oninput={(event) =>
                                        onupdate(row.id, {
                                            rootKey: (event.currentTarget as HTMLInputElement).valueAsNumber,
                                        })}
                                />
                                <small>{noteName(row.rootKey)}</small></span
                            >
                            <button
                                class="settings-toggle"
                                type="button"
                                aria-expanded={row.settingsExpanded}
                                onclick={() => onupdate(row.id, { settingsExpanded: !row.settingsExpanded })}
                            >
                                Settings
                            </button>
                        {:else}
                            <span class="audio-import-unavailable" aria-hidden="true">—</span>
                        {/if}
                    </td>
                    <td class="status-cell" data-label="Status">
                        {#if row.status === 'waiting'}
                            <span class="status-neutral">Checking…</span>
                        {:else if row.status === 'uploading'}
                            <span class="status-neutral">Uploading {Math.round(row.progress * 100)}%</span>
                        {:else if row.status === 'checking'}
                            <span class="status-neutral">Checking…</span>
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
                    </td>
                    <td class="action-cell" data-label="Actions">
                        {#if ['ready', 'failed'].includes(row.status) && validationError}
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
                    </td>
                </tr>
                {#if editable && row.settingsExpanded}
                    <tr class="settings-row">
                        <td colspan="8">
                            <AudioSamplerSettings {row} disabled={busy} {onupdate} />
                        </td>
                    </tr>
                {/if}
            {/each}
        </tbody>
    </table>
</div>

<style>
    .audio-import-summary {
        margin: 0;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .audio-import-rows {
        min-height: 0;
        overflow-y: auto;
        padding-right: 4px;
    }
    .audio-import-table {
        width: 100%;
        table-layout: fixed;
        border-collapse: separate;
        border-spacing: 0 7px;
        color: var(--color-text);
        font-size: 12px;
    }
    .source-column {
        width: 19%;
    }
    .rate-column {
        width: 11%;
    }
    .sample-column {
        width: 13%;
    }
    .wave-column {
        width: 14%;
    }
    .root-key-column {
        width: 7%;
    }
    .status-column {
        width: 18%;
    }
    .action-column {
        width: 4%;
    }
    th {
        position: sticky;
        z-index: 1;
        top: 0;
        padding: 0 9px 3px;
        color: var(--color-text-muted);
        background: var(--color-panel);
        font-size: 12px;
        font-weight: 500;
        text-align: left;
    }
    td {
        min-width: 0;
        padding: 9px;
        vertical-align: middle;
        border-top: 1px solid var(--color-border);
        border-bottom: 1px solid var(--color-border);
        background: rgb(255 255 255 / 1.5%);
    }
    td:first-child {
        border-left: 1px solid var(--color-border);
        border-radius: 5px 0 0 5px;
    }
    td:last-child {
        border-right: 1px solid var(--color-border);
        border-radius: 0 5px 5px 0;
    }
    .audio-import-file {
        min-width: 0;
    }
    .audio-import-file strong {
        display: -webkit-box;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 12px;
        font-weight: 600;
        line-height: 1.35;
        overflow-wrap: anywhere;
        -webkit-box-orient: vertical;
        -webkit-line-clamp: 2;
        line-clamp: 2;
    }
    .audio-import-file small {
        display: block;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        margin-top: 4px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    td input,
    td select {
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
    td select {
        appearance: auto;
    }
    td input:focus,
    td select:focus {
        border-color: var(--color-accent);
    }
    .root-key-control {
        display: flex;
        align-items: center;
        gap: 6px;
    }
    .root-key-control input {
        width: 54px;
        flex: none;
    }
    .root-key-control small {
        color: var(--color-text-muted);
        font-size: 11px;
        white-space: nowrap;
    }
    .settings-toggle {
        margin-top: 5px;
        padding: 0;
        color: var(--color-text-muted);
        border: 0;
        background: transparent;
        font: inherit;
        font-size: 11px;
        cursor: pointer;
    }
    .settings-toggle:hover,
    .settings-toggle[aria-expanded='true'] {
        color: var(--color-text-strong);
    }
    .settings-row td {
        padding: 10px 12px;
        border: 1px solid var(--color-border);
        border-top: 0;
        border-radius: 0 0 5px 5px;
        background: rgb(255 255 255 / 2.5%);
    }
    .audio-import-unavailable {
        display: flex;
        align-items: center;
        height: var(--density-control);
        color: var(--color-text-muted);
    }
    .error-text {
        color: var(--color-danger) !important;
    }
    .status-cell {
        overflow-wrap: anywhere;
    }
    .status-message {
        display: flex;
        align-items: flex-start;
        gap: 6px;
        font-size: 11px;
        line-height: 1.35;
    }
    .status-message :global(svg) {
        flex: none;
        margin-top: 1px;
    }
    .status-valid {
        color: var(--color-success);
    }
    .status-error {
        color: var(--color-danger);
    }
    .status-neutral {
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .action-cell {
        text-align: center;
    }
    .row-remove-button {
        margin: 0 auto;
        color: var(--color-danger);
    }
    .visually-hidden {
        position: absolute;
        width: 1px;
        height: 1px;
        padding: 0;
        margin: -1px;
        overflow: hidden;
        clip: rect(0, 0, 0, 0);
        white-space: nowrap;
        border: 0;
    }
    @media (max-width: 900px) {
        .audio-import-table,
        .audio-import-table tbody {
            display: block;
        }
        .audio-import-table {
            border-spacing: 0;
        }
        colgroup,
        thead {
            display: none;
        }
        tbody {
            display: grid !important;
            gap: 8px;
        }
        tr {
            display: grid;
            grid-template-columns: repeat(2, minmax(0, 1fr));
            gap: 9px 12px;
            padding: 10px;
            border: 1px solid var(--color-border);
            border-radius: 5px;
            background: rgb(255 255 255 / 1.5%);
        }
        td,
        td:first-child,
        td:last-child {
            display: grid;
            gap: 4px;
            padding: 0;
            border: 0;
            border-radius: 0;
            background: transparent;
        }
        td::before {
            content: attr(data-label);
            color: var(--color-text-muted);
            font-size: 12px;
        }
        .source-cell,
        .status-cell {
            grid-column: 1 / -1;
        }
        .action-cell {
            display: flex;
            align-items: flex-end;
            justify-content: flex-end;
        }
    }
    @media (max-width: 620px) {
        tr {
            grid-template-columns: minmax(0, 1fr);
        }
        .source-cell,
        .status-cell {
            grid-column: auto;
        }
    }
</style>
