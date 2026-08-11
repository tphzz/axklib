<script lang="ts">
    import { onDestroy, untrack } from 'svelte';
    import { defaultAudioImportNames, defaultAudioSamplerSettings, validSamplerName } from '../audioImport';
    import { AudioImportAuditionController, type AudioImportAuditionState } from '../audio/audioImportAudition';
    import { browserUploadSource, type ClientUploadSource } from '../clientUploadSource';
    import { modal } from '../modal';
    import type { FileLocation, InputFileLocation } from '../storageLocations';
    import type {
        AudioImportCapabilities,
        AudioImportGrouping,
        AudioImportItem,
        AudioImportTarget,
        AudioSourceInfo,
        ImageTransport,
    } from '../transport';
    import AudioImportRows from './AudioImportRows.svelte';
    import type { AudioImportRow } from './audioImportDialogTypes';
    import Icon from './Icon.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';

    interface Props {
        transport: ImageTransport;
        files: (File | ClientUploadSource | FileLocation)[];
        target: AudioImportTarget;
        existingSampleNames: string[];
        existingSampleBankNames?: string[];
        existingWaveformNames: string[];
        onchooseworkspace?: () => void;
        onchooselocal?: () => void;
        oncommit: (items: AudioImportItem[], grouping: AudioImportGrouping) => Promise<void>;
        oncancel: () => void;
    }

    let {
        transport,
        files,
        target,
        existingSampleNames,
        existingSampleBankNames = [],
        existingWaveformNames,
        onchooseworkspace,
        onchooselocal,
        oncommit,
        oncancel,
    }: Props = $props();
    let rows = $state<AudioImportRow[]>([]);
    let busy = $state(false);
    let committing = $state(false);
    let batchStaging = $state(false);
    let generalError = $state('');
    let stagingError = $state('');
    let importMode = $state<AudioImportGrouping['kind']>('SAMPLES');
    let sampleBankName = $state('');
    let nextRowId = 0;
    let audioImportCapabilities = $state<AudioImportCapabilities>();
    let auditionState = $state<AudioImportAuditionState>({ rowId: null, status: 'idle', error: '' });
    let disposed = false;
    let stagingPromise: Promise<void> = Promise.resolve();
    const abortController = new AbortController();
    const auditionController = new AudioImportAuditionController((state) => (auditionState = state));
    const validationErrors = $derived.by(() => (committing ? rows.map(() => '') : validateRows(rows)));
    const sampleBankError = $derived.by(() => validateSampleBank());
    const inspectedCount = $derived(rows.filter((row) => row.status === 'inspected' || row.status === 'failed').length);
    const ready = $derived(
        rows.length > 0 &&
            rows.every((row) => row.status === 'ready') &&
            validationErrors.every((error) => error === '') &&
            sampleBankError === '',
    );

    function validateSampleBank(): string {
        if (importMode !== 'SAMPLE_BANK') return '';
        if (!validSamplerName(sampleBankName)) {
            return 'Sample Bank name must be 1-16 printable ASCII characters.';
        }
        if (existingSampleBankNames.some((name) => name.toLocaleLowerCase() === sampleBankName.toLocaleLowerCase())) {
            return `Sample Bank name already exists: ${sampleBankName}`;
        }
        if (rows.length > 127) return 'A Sample Bank can contain at most 127 Samples.';
        return '';
    }

    function isWorkspaceFile(candidate: ClientUploadSource | FileLocation): candidate is FileLocation {
        return 'kind' in candidate && candidate.kind === 'server-file';
    }

    $effect(() => {
        stagingError = '';
        rows = files.map((input) => {
            const candidate = 'kind' in input ? input : 'readChunk' in input ? input : browserUploadSource(input);
            const fileName = isWorkspaceFile(candidate)
                ? (candidate.reference.relativePath.split('/').at(-1) ?? candidate.displayName)
                : candidate.name;
            return {
                id: nextRowId++,
                candidate,
                fileName,
                sampleName: '',
                waveformNames: [],
                ...defaultAudioSamplerSettings,
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
        void auditionController.dispose();
        if (!busy) void stagingPromise.finally(() => releaseUploads());
    });

    function replaceRow(id: number, update: Partial<AudioImportRow>): void {
        const index = rows.findIndex((row) => row.id === id);
        if (index >= 0) rows[index] = { ...rows[index], ...update };
    }

    async function stageOne(id: number): Promise<void> {
        const row = rows.find((candidate) => candidate.id === id);
        if (!row) return;
        try {
            let source: InputFileLocation;
            if (isWorkspaceFile(row.candidate)) {
                source = row.candidate;
                replaceRow(id, { source, status: 'checking' });
            } else {
                replaceRow(id, { status: 'uploading' });
                const upload = await transport.uploadClientFile(
                    row.candidate,
                    'AUDIO',
                    (sent, total) => replaceRow(id, { progress: total === 0 ? 0 : sent / total }),
                    abortController.signal,
                );
                source = upload;
                replaceRow(id, { source, upload, progress: 1, status: 'checking' });
            }
            const inspection = await transport.inspectAudio(source);
            replaceRow(id, {
                inspection,
                targetSampleRate: inspection.outputSampleRate,
                ...editableSamplerSettings(inspection),
                status: 'inspected',
            });
        } catch (error) {
            if (!abortController.signal.aborted) {
                replaceRow(id, { status: 'failed', error: error instanceof Error ? error.message : String(error) });
            }
        }
    }

    function editableSamplerSettings(inspection: AudioSourceInfo): Partial<AudioImportRow> {
        const defaults = inspection.samplerDefaults;
        return {
            rootKey: defaults.rootKey,
            fineTuneCents: defaults.fineTuneCents,
            keyLow: defaults.keyLow,
            keyHigh: defaults.keyHigh,
            velocityLow: defaults.velocityLow,
            velocityHigh: defaults.velocityHigh,
            loopMode: defaults.loopMode,
            loopStartFrame: defaults.loopStartFrame,
            loopLengthFrames: defaults.loopLengthFrames,
        };
    }

    async function stageFiles(): Promise<void> {
        if (rows.length === 0) return;
        batchStaging = true;
        try {
            audioImportCapabilities = await transport.audioImportCapabilities();
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            rows = rows.map((row) => ({ ...row, status: 'failed', error: message }));
            batchStaging = false;
            return;
        }
        const uploadCount = rows.filter((row) => !isWorkspaceFile(row.candidate)).length;
        if (uploadCount > audioImportCapabilities.maximumUploads) {
            stagingError = `This server can stage at most ${audioImportCapabilities.maximumUploads} local files at once; ${uploadCount} were selected.`;
            batchStaging = false;
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
        rows = rows.map((row) => {
            if (row.status !== 'inspected' || !row.inspection) return row;
            if (!row.inspection.valid) return { ...row, status: 'ready' };
            return {
                ...row,
                ...defaultAudioImportNames(row.fileName, row.inspection, usedSamples, usedWaveforms),
                status: 'ready',
            };
        });
        batchStaging = false;
    }

    function validateRows(items: AudioImportRow[]): string[] {
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
            if (!Number.isInteger(row.rootKey) || row.rootKey < 0 || row.rootKey > 127) {
                errors[index] = 'Root key must be between 0 and 127.';
                return;
            }
            if (!Number.isInteger(row.fineTuneCents) || row.fineTuneCents < -63 || row.fineTuneCents > 63) {
                errors[index] = 'Fine tune must be between -63 and 63 cents.';
                return;
            }
            if (
                !Number.isInteger(row.keyLow) ||
                !Number.isInteger(row.keyHigh) ||
                row.keyLow < 0 ||
                row.keyHigh > 127 ||
                row.keyLow > row.keyHigh
            ) {
                errors[index] = 'Key range must be an ordered range from 0 to 127.';
                return;
            }
            if (
                !Number.isInteger(row.velocityLow) ||
                !Number.isInteger(row.velocityHigh) ||
                row.velocityLow < 0 ||
                row.velocityHigh > 127 ||
                row.velocityLow > row.velocityHigh
            ) {
                errors[index] = 'Velocity range must be an ordered range from 0 to 127.';
                return;
            }
            if (![1, 4].includes(row.loopMode)) {
                errors[index] = 'Loop mode must be forward loop or one-shot.';
                return;
            }
            if (row.loopMode === 4 && (row.loopStartFrame !== 0 || row.loopLengthFrames !== 0)) {
                errors[index] = 'One-shot imports cannot contain an active loop range.';
                return;
            }
            if (
                row.loopMode === 1 &&
                (!Number.isInteger(row.loopStartFrame) ||
                    !Number.isInteger(row.loopLengthFrames) ||
                    row.loopStartFrame < 0 ||
                    row.loopLengthFrames <= 0 ||
                    row.loopStartFrame + row.loopLengthFrames > row.inspection.projectedOutputFrameCount)
            ) {
                errors[index] = 'Forward loop must be non-empty and contained within the converted audio.';
            }
        });
        return errors;
    }

    async function changeTargetSampleRate(row: AudioImportRow, event: Event): Promise<void> {
        const targetSampleRate = Number((event.currentTarget as HTMLSelectElement).value);
        if (!row.source || !Number.isInteger(targetSampleRate) || row.targetSampleRate === targetSampleRate) return;
        auditionController.stop(row.id);
        const revision = row.inspectionRevision + 1;
        replaceRow(row.id, { targetSampleRate, inspectionRevision: revision, status: 'checking', error: '' });
        try {
            const inspection = await transport.inspectAudio(row.source, targetSampleRate);
            const current = rows.find((candidate) => candidate.id === row.id);
            if (!current || current.inspectionRevision !== revision || disposed) return;
            replaceRow(row.id, { inspection, ...editableSamplerSettings(inspection), status: 'ready' });
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
        await auditionController.dispose();
        await releaseUploads();
        oncancel();
    }

    async function removeRow(row: AudioImportRow): Promise<void> {
        if (busy || !['ready', 'failed'].includes(row.status)) return;
        auditionController.stop(row.id);
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
        auditionController.stop();
        busy = true;
        committing = true;
        generalError = '';
        try {
            const items = rows.map((row) => ({
                source: row.source!,
                sampleName: row.sampleName,
                waveformNames: [...row.waveformNames],
                rootKey: row.rootKey,
                fineTuneCents: row.fineTuneCents,
                keyLow: row.keyLow,
                keyHigh: row.keyHigh,
                velocityLow: row.velocityLow,
                velocityHigh: row.velocityHigh,
                loopMode: row.loopMode,
                loopStartFrame: row.loopStartFrame,
                loopLengthFrames: row.loopLengthFrames,
                targetSampleRate: row.targetSampleRate!,
            }));
            await oncommit(
                items,
                importMode === 'SAMPLE_BANK' ? { kind: 'SAMPLE_BANK', sampleBankName } : { kind: 'SAMPLES' },
            );
            await releaseUploads();
            oncancel();
        } catch (error) {
            generalError = error instanceof Error ? error.message : String(error);
            committing = false;
            busy = false;
        }
    }

    function updateRow(id: number, update: Partial<AudioImportRow>): void {
        if (
            'loopMode' in update ||
            'loopStartFrame' in update ||
            'loopLengthFrames' in update ||
            'targetSampleRate' in update
        ) {
            auditionController.stop(id);
        }
        replaceRow(id, update);
    }

    async function loadAuditionBlob(row: AudioImportRow): Promise<Blob> {
        if (isWorkspaceFile(row.candidate)) return (await transport.downloadFile(row.candidate)).blob;
        return row.candidate.readChunk(0, row.candidate.size);
    }

    function audition(row: AudioImportRow): void {
        if (auditionState.rowId === row.id && ['preparing', 'playing'].includes(auditionState.status)) {
            auditionController.stop(row.id);
            return;
        }
        if (!row.inspection || row.targetSampleRate === undefined) return;
        void auditionController.play({
            rowId: row.id,
            loadBlob: () => loadAuditionBlob(row),
            timeline: {
                frameCount: row.inspection!.projectedOutputFrameCount,
                sampleRate: row.targetSampleRate!,
                loopMode: row.loopMode,
                loopStartFrame: row.loopStartFrame,
                loopLengthFrames: row.loopLengthFrames,
            },
        });
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
            {#if files.length === 0}
                <ImportSourceChoice
                    label="Audio source"
                    heading="Choose audio files"
                    description={`Import into ${target.volumeName} from a configured storage location or this computer.`}
                    workspaceDetail="Choose one or more files from a configured workspace"
                    computerDetail="Choose local audio files and upload them"
                    computerAvailable={onchooselocal !== undefined}
                    onchooseworkspace={() => onchooseworkspace?.()}
                    onchooselocal={() => onchooselocal?.()}
                />
            {:else}
                <div class="import-target-settings">
                    <label>
                        <span>Import mode</span>
                        <select bind:value={importMode} disabled={busy}>
                            <option value="SAMPLES">Import as Samples</option>
                            <option value="SAMPLE_BANK">Import as Samples in a Sample Bank</option>
                        </select>
                    </label>
                    {#if importMode === 'SAMPLE_BANK'}
                        <label>
                            <span>Sample Bank name</span>
                            <input
                                aria-invalid={sampleBankError !== ''}
                                bind:value={sampleBankName}
                                maxlength="16"
                                autocomplete="off"
                                disabled={busy}
                            />
                        </label>
                        {#if sampleBankError}<p class="field-error" role="alert">{sampleBankError}</p>{/if}
                    {/if}
                </div>
                {#if stagingError}
                    <p class="dialog-error" role="alert">{stagingError}</p>
                {:else}
                    {#if batchStaging}
                        <div class="inspection-progress">
                            <span>Inspecting {inspectedCount} of {rows.length} files</span>
                            <progress aria-label="Inspecting audio files" value={inspectedCount} max={rows.length}
                            ></progress>
                        </div>
                    {/if}
                    <AudioImportRows
                        {rows}
                        {validationErrors}
                        capabilities={audioImportCapabilities}
                        {busy}
                        {committing}
                        grouped={importMode === 'SAMPLE_BANK'}
                        audition={auditionState}
                        onchangeTargetSampleRate={(row, event) => void changeTargetSampleRate(row, event)}
                        onupdate={updateRow}
                        onremove={(row) => void removeRow(row)}
                        onaudition={audition}
                    />
                {/if}
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
    .inspection-progress {
        display: grid;
        gap: 5px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .import-target-settings {
        display: grid;
        grid-template-columns: minmax(240px, 360px);
        gap: 8px;
    }
    .import-target-settings label {
        display: grid;
        gap: 5px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .import-target-settings select,
    .import-target-settings input {
        width: 100%;
    }
    .field-error {
        margin: 0;
        color: var(--color-danger);
        font-size: 11px;
    }
    .inspection-progress progress {
        width: 100%;
        height: 4px;
        accent-color: var(--color-accent);
    }
    @media (max-width: 900px) {
        .audio-import-dialog {
            width: calc(100vw - 24px);
        }
    }
</style>
