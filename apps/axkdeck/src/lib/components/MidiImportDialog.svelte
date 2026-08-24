<script lang="ts">
    import { onDestroy, untrack } from 'svelte';
    import { validSamplerName } from '../audioImport';
    import { browserUploadSource, type ClientUploadSource } from '../clientUploadSource';
    import { defaultSequenceName } from '../midiImport';
    import { modal } from '../modal';
    import type { ClientUploadLocation, FileLocation, InputFileLocation } from '../storageLocations';
    import type {
        ImageTransport,
        MidiInspection,
        SequenceImportItem,
        SequenceImportTarget,
        SequenceSystemExclusivePolicy,
    } from '../transport';
    import Icon from './Icon.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';

    interface Props {
        transport: ImageTransport;
        files: (File | ClientUploadSource | FileLocation)[];
        target: SequenceImportTarget;
        existingSequenceNames: string[];
        onchooseworkspace?: () => void;
        onchooselocal?: () => void;
        oncommit: (items: SequenceImportItem[], systemExclusivePolicy: SequenceSystemExclusivePolicy) => Promise<void>;
        oncancel: () => void;
    }

    interface Row {
        id: number;
        candidate: ClientUploadSource | FileLocation;
        fileName: string;
        sequenceName: string;
        source?: InputFileLocation;
        upload?: ClientUploadLocation;
        inspection?: MidiInspection;
        progress: number;
        status: 'waiting' | 'uploading' | 'inspecting' | 'ready' | 'failed' | 'removing';
        error: string;
    }

    let {
        transport,
        files,
        target,
        existingSequenceNames,
        onchooseworkspace,
        onchooselocal,
        oncommit,
        oncancel,
    }: Props = $props();
    let rows = $state<Row[]>([]);
    let busy = $state(false);
    let generalError = $state('');
    let includeSystemExclusive = $state(false);
    let nextRowId = 0;
    let disposed = false;
    let stagingPromise: Promise<void> = Promise.resolve();
    const abortController = new AbortController();
    const validationErrors = $derived(validateRows(rows, existingSequenceNames));
    const ready = $derived(
        rows.length > 0 &&
            rows.every((row) => row.status === 'ready' && row.source !== undefined && row.inspection !== undefined) &&
            validationErrors.every((error) => error === ''),
    );
    const systemExclusiveEventCount = $derived(
        rows.reduce((total, row) => total + (row.inspection?.systemExclusiveEventCount ?? 0), 0),
    );
    const systemExclusivePreservationSupported = $derived(
        systemExclusiveEventCount > 0 &&
            rows
                .filter((row) => (row.inspection?.systemExclusiveEventCount ?? 0) > 0)
                .every((row) => row.inspection?.systemExclusivePreservationSupported === true),
    );

    $effect(() => {
        if (!systemExclusivePreservationSupported) includeSystemExclusive = false;
    });

    function isWorkspaceFile(candidate: ClientUploadSource | FileLocation): candidate is FileLocation {
        return 'kind' in candidate && candidate.kind === 'server-file';
    }

    function sourceName(candidate: ClientUploadSource | FileLocation): string {
        return isWorkspaceFile(candidate)
            ? (candidate.reference.relativePath.split('/').at(-1) ?? candidate.displayName)
            : candidate.name;
    }

    $effect(() => {
        const usedNames = new Set(existingSequenceNames.map((name) => name.toLocaleLowerCase()));
        rows = files.map((input) => {
            const candidate = 'kind' in input ? input : 'readChunk' in input ? input : browserUploadSource(input);
            const fileName = sourceName(candidate);
            return {
                id: nextRowId++,
                candidate,
                fileName,
                sequenceName: defaultSequenceName(fileName, usedNames),
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
        try {
            if (isWorkspaceFile(row.candidate)) {
                replaceRow(id, { source: row.candidate, progress: 1, status: 'inspecting' });
                const inspection = await transport.inspectMidi(row.candidate);
                if (!disposed) replaceRow(id, { inspection, status: 'ready' });
                return;
            }
            replaceRow(id, { status: 'uploading' });
            const upload = await transport.uploadClientFile(
                row.candidate,
                'MIDI',
                (sent, total) => replaceRow(id, { progress: total === 0 ? 0 : sent / total }),
                abortController.signal,
            );
            if (disposed || abortController.signal.aborted) {
                await transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            replaceRow(id, { source: upload, upload, progress: 1, status: 'inspecting' });
            const inspection = await transport.inspectMidi(upload);
            if (!disposed) replaceRow(id, { inspection, status: 'ready' });
        } catch (error) {
            if (!abortController.signal.aborted) {
                replaceRow(id, { status: 'failed', error: error instanceof Error ? error.message : String(error) });
            }
        }
    }

    async function stageFiles(): Promise<void> {
        const ids = rows.map((row) => row.id);
        const workers = Array.from({ length: Math.min(3, ids.length) }, async (_, worker) => {
            for (let index = worker; index < ids.length; index += 3) await stageOne(ids[index]);
        });
        await Promise.all(workers);
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
        if (rows.length === 0) oncancel();
    }

    async function commit(): Promise<void> {
        if (!ready || busy) return;
        busy = true;
        generalError = '';
        try {
            await oncommit(
                rows.map((row) => ({ source: row.source!, sequenceName: row.sequenceName })),
                includeSystemExclusive ? 'preserve' : 'exclude',
            );
            await releaseUploads();
            oncancel();
        } catch (error) {
            generalError = error instanceof Error ? error.message : String(error);
            busy = false;
        }
    }

    function validateRows(items: Row[], existingNames: string[]): string[] {
        const names = new Set(existingNames.map((name) => name.toLocaleLowerCase()));
        return items.map((row) => {
            if (row.status === 'failed') return row.error;
            if (!validSamplerName(row.sequenceName)) return 'Sequence names must be 1-16 printable ASCII characters.';
            const key = row.sequenceName.toLocaleLowerCase();
            if (names.has(key)) return `Sequence name already exists: ${row.sequenceName}`;
            names.add(key);
            return '';
        });
    }

    function inspectionSummary(row: Row): string {
        if (!row.inspection) return '';
        const controllers = row.inspection.controllers.map((entry) => entry.controller);
        const details = [
            controllers.length > 0 ? `CC ${controllers.join(', ')}` : '',
            row.inspection.systemExclusiveEventCount > 0 ? `${row.inspection.systemExclusiveEventCount} SysEx` : '',
        ].filter(Boolean);
        return details.length > 0 ? `Ready · ${details.join(' · ')}` : 'Ready';
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide midi-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import MIDI"
        use:modal={{ onescape: () => void cancel() }}
    >
        <header class="dialog-header">
            <div>
                <h2>Import MIDI</h2>
                <p>Volume {target.volumeName}</p>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={() => void cancel()}>
                <Icon name="close" size={15} />
            </button>
        </header>
        <div class="midi-import-body">
            {#if files.length === 0}
                <ImportSourceChoice
                    label="MIDI source"
                    heading="Choose MIDI files"
                    description={`Import Sequences into ${target.volumeName} from a configured storage location or this computer.`}
                    workspaceDetail="Choose one or more MIDI files from a configured workspace"
                    computerDetail="Choose local MIDI files and upload them"
                    computerAvailable={onchooselocal !== undefined}
                    onchooseworkspace={() => onchooseworkspace?.()}
                    onchooselocal={() => onchooselocal?.()}
                />
            {:else}
                <div class="midi-import-list" role="table" aria-label="MIDI files">
                    <div class="midi-import-head" role="row">
                        <span role="columnheader">Source file</span>
                        <span role="columnheader">Sequence name</span>
                        <span role="columnheader">Status</span>
                        <span aria-hidden="true"></span>
                    </div>
                    {#each rows as row, index (row.id)}
                        <div class="midi-import-row" role="row">
                            <strong title={row.fileName}>{row.fileName}</strong>
                            <input
                                aria-label={`Sequence name for ${row.fileName}`}
                                data-dialog-initial-focus={index === 0 ? 'select' : undefined}
                                value={row.sequenceName}
                                maxlength="16"
                                disabled={busy}
                                oninput={(event) => replaceRow(row.id, { sequenceName: event.currentTarget.value })}
                            />
                            <span class:error={validationErrors[index]}>
                                {#if row.status === 'uploading'}
                                    Uploading {Math.round(row.progress * 100)}%
                                {:else if row.status === 'inspecting'}
                                    Inspecting
                                {:else if row.status === 'ready'}
                                    {inspectionSummary(row)}
                                {:else if row.status === 'failed'}
                                    {row.error}
                                {:else}
                                    Preparing
                                {/if}
                            </span>
                            <button
                                class="icon-button"
                                type="button"
                                aria-label={`Remove ${row.fileName}`}
                                disabled={busy || !['ready', 'failed'].includes(row.status)}
                                onclick={() => void removeRow(row)}
                            >
                                <Icon name="trash" size={13} />
                            </button>
                        </div>
                        {#if validationErrors[index] && row.status !== 'failed'}
                            <p class="midi-row-error" role="alert">{validationErrors[index]}</p>
                        {/if}
                    {/each}
                </div>
                <section class="system-exclusive-options" aria-label="System Exclusive import">
                    <label>
                        <input
                            class="dialog-checkbox"
                            type="checkbox"
                            aria-label="Include SysEx events"
                            checked={includeSystemExclusive}
                            disabled={busy || systemExclusiveEventCount === 0 || !systemExclusivePreservationSupported}
                            onchange={(event) => (includeSystemExclusive = event.currentTarget.checked)}
                        />
                        <span>Include SysEx events</span>
                    </label>
                    {#if systemExclusiveEventCount === 0}
                        <p>No SysEx events found.</p>
                    {:else if !systemExclusivePreservationSupported}
                        <p>
                            {systemExclusiveEventCount} SysEx {systemExclusiveEventCount === 1 ? 'event' : 'events'}
                            will be excluded. At least one event is outside the supported preservation profile.
                        </p>
                    {:else if includeSystemExclusive}
                        <p>
                            {systemExclusiveEventCount} SysEx {systemExclusiveEventCount === 1 ? 'event' : 'events'}
                            will be included.
                        </p>
                    {:else}
                        <p>
                            {systemExclusiveEventCount} SysEx {systemExclusiveEventCount === 1 ? 'event' : 'events'}
                            will be excluded.
                        </p>
                    {/if}
                </section>
            {/if}
            {#if generalError}<p class="dialog-error" role="alert">{generalError}</p>{/if}
        </div>
        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={busy} onclick={() => void cancel()}>Cancel</button>
            {#if files.length > 0}
                <button class="primary-button" type="button" disabled={!ready || busy} onclick={() => void commit()}>
                    {busy ? 'Importing' : `Import ${rows.length} ${rows.length === 1 ? 'file' : 'files'}`}
                </button>
            {/if}
        </footer>
    </div>
</div>

<style>
    .midi-import-dialog {
        width: min(760px, calc(100vw - 32px));
        max-width: none;
        max-height: min(720px, calc(100vh - 48px));
    }
    .dialog-header p {
        margin: 2px 0 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .midi-import-body {
        min-height: 0;
        overflow: auto;
        display: flex;
        flex-direction: column;
        padding: 12px;
        gap: 10px;
    }
    .midi-import-list {
        display: grid;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        overflow: hidden;
    }
    .midi-import-head,
    .midi-import-row {
        display: grid;
        grid-template-columns: minmax(160px, 1.5fr) minmax(140px, 1fr) minmax(90px, 1fr) 28px;
        align-items: center;
        gap: 8px;
        min-height: 36px;
        padding: 4px 8px;
    }
    .midi-import-head {
        color: var(--color-text-muted);
        border-bottom: 1px solid var(--color-border);
        font-size: var(--dialog-table-header-font-size);
    }
    .midi-import-row {
        border-bottom: 1px solid var(--color-border-subtle);
        font-size: var(--dialog-body-font-size);
    }
    .midi-import-row:last-of-type {
        border-bottom: 0;
    }
    .midi-import-row strong {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .midi-import-row input {
        min-width: 0;
        height: 27px;
        padding: 0 7px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 4px;
        background: var(--color-bg-deep);
        font-family: var(--font-mono);
        font-size: var(--dialog-control-font-size);
    }
    .midi-import-row span {
        color: var(--color-success);
        overflow-wrap: anywhere;
    }
    .midi-import-row span.error,
    .midi-row-error {
        color: var(--color-danger);
    }
    .midi-row-error {
        margin: 0;
        padding: 0 8px 6px;
        font-size: var(--dialog-metadata-font-size);
    }
    .system-exclusive-options {
        display: grid;
        gap: 3px;
        padding: 8px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
    }
    .system-exclusive-options label {
        display: flex;
        align-items: center;
        gap: 7px;
        font-size: var(--dialog-body-font-size);
    }
    .system-exclusive-options input {
        width: 14px;
        height: 14px;
        margin: 0;
    }
    .system-exclusive-options p {
        margin: 0 0 0 21px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
</style>
