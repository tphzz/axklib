<script lang="ts">
    import { onDestroy, untrack } from 'svelte';
    import { defaultAudioImportNames, defaultRootKey, noteName, validSamplerName } from '../audioImport';
    import { browserUploadSource, type ClientUploadSource } from '../clientUploadSource';
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { ClientUploadLocation } from '../storageLocations';
    import type {
        AudioImportCapabilities,
        AudioImportItem,
        AudioImportTarget,
        AudioSourceInfo,
        ImageTransport,
    } from '../transport';
    import Icon from './Icon.svelte';

    interface Props {
        transport: ImageTransport;
        files: (File | ClientUploadSource)[];
        target: AudioImportTarget;
        existingSampleNames: string[];
        existingWaveformNames: string[];
        oncommit: (items: AudioImportItem[]) => Promise<void>;
        oncancel: () => void;
    }

    interface Row {
        id: number;
        file: ClientUploadSource;
        upload?: ClientUploadLocation;
        inspection?: AudioSourceInfo;
        targetSampleRate?: number;
        inspectionRevision: number;
        sampleName: string;
        waveformNames: string[];
        rootKey: number;
        progress: number;
        status: 'waiting' | 'uploading' | 'checking' | 'ready' | 'failed' | 'removing';
        error: string;
    }

    let { transport, files, target, existingSampleNames, existingWaveformNames, oncommit, oncancel }: Props = $props();
    let rows = $state<Row[]>([]);
    let busy = $state(false);
    let generalError = $state('');
    let nextRowId = 0;
    let audioImportCapabilities = $state<AudioImportCapabilities>();
    let disposed = false;
    let stagingPromise: Promise<void> = Promise.resolve();
    const abortController = new AbortController();
    const validationErrors = $derived.by(() => validateRows(rows));
    const ready = $derived(
        rows.length > 0 &&
            rows.every((row) => row.status === 'ready') &&
            validationErrors.every((error) => error === ''),
    );

    $effect(() => {
        rows = files.map((input) => {
            const file = 'readChunk' in input ? input : browserUploadSource(input);
            return {
                id: nextRowId++,
                file,
                sampleName: '',
                waveformNames: [],
                rootKey: defaultRootKey,
                inspectionRevision: 0,
                progress: 0,
                status: 'waiting',
                error: '',
            };
        });
        untrack(() => {
            stagingPromise = stageFiles();
        });
    });

    onDestroy(() => {
        disposed = true;
        abortController.abort();
        if (!busy) void stagingPromise.finally(() => releaseUploads());
    });

    function replaceRow(id: number, update: Partial<Row>): void {
        const index = rows.findIndex((row) => row.id === id);
        if (index >= 0) rows[index] = { ...rows[index], ...update };
    }

    async function stageOne(id: number): Promise<void> {
        const row = rows.find((candidate) => candidate.id === id);
        if (!row) return;
        replaceRow(id, { status: 'uploading' });
        try {
            const upload = await transport.uploadClientFile(
                row.file,
                'AUDIO',
                (sent, total) => replaceRow(id, { progress: total === 0 ? 0 : sent / total }),
                abortController.signal,
            );
            replaceRow(id, { upload, progress: 1 });
            const inspection = await transport.inspectAudio(upload);
            replaceRow(id, { inspection, targetSampleRate: inspection.outputSampleRate, status: 'ready' });
        } catch (error) {
            if (!abortController.signal.aborted) {
                replaceRow(id, { status: 'failed', error: error instanceof Error ? error.message : String(error) });
            }
        }
    }

    async function stageFiles(): Promise<void> {
        try {
            audioImportCapabilities = await transport.audioImportCapabilities();
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            rows = rows.map((row) => ({ ...row, status: 'failed', error: message }));
            return;
        }
        const ids = rows.map((row) => row.id);
        const workers = Array.from({ length: Math.min(3, ids.length) }, async (_, worker) => {
            for (let index = worker; index < ids.length; index += 3) await stageOne(ids[index]);
        });
        await Promise.all(workers);
        if (disposed || abortController.signal.aborted) return;
        const usedSamples = new Set(existingSampleNames.map((name) => name.toLocaleLowerCase()));
        const usedWaveforms = new Set(existingWaveformNames.map((name) => name.toLocaleLowerCase()));
        rows.forEach((row) => {
            if (!row.inspection?.valid) return;
            const names = defaultAudioImportNames(row.file.name, row.inspection, usedSamples, usedWaveforms);
            replaceRow(row.id, names);
        });
    }

    function validateRows(items: Row[]): string[] {
        const errors = items.map(() => '');
        const samples = new Set(existingSampleNames.map((name) => name.toLocaleLowerCase()));
        const waveforms = new Set(existingWaveformNames.map((name) => name.toLocaleLowerCase()));
        items.forEach((row, index) => {
            if (row.status === 'failed') {
                errors[index] = row.error;
                return;
            }
            if (row.status !== 'ready' || !row.inspection) return;
            const admissionIssue = row.inspection.issues.find((issue) => issue.fatal !== false);
            if (!row.inspection.valid || admissionIssue) {
                errors[index] = admissionIssue?.message ?? 'This audio file cannot be imported.';
                return;
            }
            if (!validSamplerName(row.sampleName)) {
                errors[index] = 'Sample names must be 1-16 printable ASCII characters.';
                return;
            }
            if (samples.has(row.sampleName.toLocaleLowerCase())) {
                errors[index] = `Sample name already exists: ${row.sampleName}`;
                return;
            }
            samples.add(row.sampleName.toLocaleLowerCase());
            if (row.waveformNames.length !== row.inspection.channels) {
                errors[index] = 'Wave data channel names are incomplete.';
                return;
            }
            for (const name of row.waveformNames) {
                if (!validSamplerName(name)) {
                    errors[index] = 'Wave data names must be 1-16 printable ASCII characters.';
                    return;
                }
                if (waveforms.has(name.toLocaleLowerCase())) {
                    errors[index] = `Wave data name already exists: ${name}`;
                    return;
                }
                waveforms.add(name.toLocaleLowerCase());
            }
            if (!Number.isInteger(row.rootKey) || row.rootKey < 0 || row.rootKey > 127)
                errors[index] = 'Root key must be between 0 and 127.';
        });
        return errors;
    }

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

    async function changeTargetSampleRate(row: Row, event: Event): Promise<void> {
        const targetSampleRate = Number((event.currentTarget as HTMLSelectElement).value);
        if (!row.upload || !Number.isInteger(targetSampleRate) || row.targetSampleRate === targetSampleRate) return;
        const revision = row.inspectionRevision + 1;
        replaceRow(row.id, { targetSampleRate, inspectionRevision: revision, status: 'checking', error: '' });
        try {
            const inspection = await transport.inspectAudio(row.upload, targetSampleRate);
            const current = rows.find((candidate) => candidate.id === row.id);
            if (!current || current.inspectionRevision !== revision || disposed) return;
            replaceRow(row.id, { inspection, status: 'ready' });
        } catch (error) {
            const current = rows.find((candidate) => candidate.id === row.id);
            if (!current || current.inspectionRevision !== revision || disposed) return;
            replaceRow(row.id, {
                status: 'failed',
                error: error instanceof Error ? error.message : String(error),
            });
        }
    }

    async function releaseUploads(): Promise<void> {
        const uploads = rows.flatMap((row) => (row.upload ? [row.upload] : []));
        await Promise.all(uploads.map((upload) => transport.releaseClientUpload(upload).catch(() => undefined)));
        rows = rows.map((row) => ({ ...row, upload: undefined }));
    }

    async function cancel(): Promise<void> {
        if (busy) return;
        abortController.abort();
        busy = true;
        await stagingPromise;
        await releaseUploads();
        oncancel();
    }

    async function removeRow(row: Row): Promise<void> {
        if (busy || !['ready', 'failed'].includes(row.status)) return;
        replaceRow(row.id, { status: 'removing' });
        if (row.upload) await transport.releaseClientUpload(row.upload).catch(() => undefined);
        rows = rows.filter((candidate) => candidate.id !== row.id);
        if (rows.length === 0) {
            abortController.abort();
            oncancel();
        }
    }

    async function commit(): Promise<void> {
        if (!ready || busy) return;
        busy = true;
        generalError = '';
        try {
            await oncommit(
                rows.map((row) => ({
                    source: row.upload!,
                    sampleName: row.sampleName,
                    waveformNames: [...row.waveformNames],
                    rootKey: row.rootKey,
                    targetSampleRate: row.targetSampleRate!,
                })),
            );
            await releaseUploads();
            oncancel();
        } catch (error) {
            generalError = error instanceof Error ? error.message : String(error);
            busy = false;
        }
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide audio-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import audio"
        use:modal={{ onescape: () => void cancel() }}
    >
        <header class="dialog-header">
            <div>
                <h2>Import audio</h2>
                <p>Volume {target.volumeName}</p>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={() => void cancel()}>
                <Icon name="close" size={15} />
            </button>
        </header>
        <div class="audio-import-body">
            <p class="audio-import-summary">
                Each file creates one standalone Sample and {rows.some((row) => row.inspection?.channels === 2)
                    ? 'mono or stereo'
                    : 'mono'} Wave Data. Imported wave data uses the proven full-waveform forward loop.
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
                                        <strong title={row.file.name}>{row.file.name}</strong>
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
                                    {#if row.inspection?.valid && audioImportCapabilities}
                                        <select
                                            aria-label={`Target sample rate for ${row.file.name}`}
                                            value={row.targetSampleRate}
                                            disabled={busy || row.status === 'checking'}
                                            onchange={(event) => void changeTargetSampleRate(row, event)}
                                        >
                                            {#each audioImportCapabilities.supportedSampleRates as rate (rate)}
                                                <option value={rate}>{rate.toLocaleString()} Hz</option>
                                            {/each}
                                        </select>
                                    {:else}
                                        <span class="audio-import-unavailable" aria-hidden="true">—</span>
                                    {/if}
                                </td>
                                <td data-label="Sample name">
                                    {#if editable}
                                        <input aria-label="Sample name" bind:value={row.sampleName} maxlength="16" />
                                    {:else}
                                        <span class="audio-import-unavailable" aria-hidden="true">—</span>
                                    {/if}
                                </td>
                                <td data-label="Wave data (mono/left)">
                                    {#if editable}
                                        <input
                                            aria-label="Wave data (mono/left)"
                                            bind:value={row.waveformNames[0]}
                                            maxlength="16"
                                        />
                                    {:else}
                                        <span class="audio-import-unavailable" aria-hidden="true">—</span>
                                    {/if}
                                </td>
                                <td data-label="Wave data (right)">
                                    {#if editable && row.waveformNames.length === 2}
                                        <input
                                            aria-label="Wave data (right)"
                                            bind:value={row.waveformNames[1]}
                                            maxlength="16"
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
                                                bind:value={row.rootKey}
                                                type="number"
                                                min="0"
                                                max="127"
                                                aria-label="Root key"
                                            />
                                            <small>{noteName(row.rootKey)}</small></span
                                        >
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
                                            aria-label={`Remove ${row.file.name}`}
                                            title="Remove file"
                                            disabled={busy}
                                            onclick={() => void removeRow(row)}
                                        >
                                            <Icon name="trash" size={15} />
                                        </button>
                                    {/if}
                                </td>
                            </tr>
                        {/each}
                    </tbody>
                </table>
            </div>
            {#if generalError}<p class="dialog-error" role="alert">{generalError}</p>{/if}
        </div>
        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={busy} onclick={() => void cancel()}>Cancel</button>
            <button class="primary-button" type="button" disabled={!ready || busy} onclick={() => void commit()}>
                {busy ? 'Importing' : `Import ${rows.length} ${rows.length === 1 ? 'file' : 'files'}`}
            </button>
        </footer>
    </div>
</div>

<style>
    .audio-import-dialog {
        width: min(1280px, calc(100vw - 32px));
        max-width: none;
        max-height: min(720px, calc(100vh - 48px));
    }
    .dialog-header p {
        margin: 2px 0 0;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .audio-import-body {
        min-height: 0;
        overflow: hidden;
        display: flex;
        flex-direction: column;
        padding: 12px;
        gap: 10px;
    }
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
        .audio-import-dialog {
            width: calc(100vw - 24px);
        }
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
        .source-cell {
            grid-column: 1 / -1;
        }
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
        .source-cell {
            grid-column: auto;
        }
        .status-cell {
            grid-column: auto;
        }
    }
</style>
