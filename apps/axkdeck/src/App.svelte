<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import { AuditionWorkflow } from './features/audition/workflow.svelte';
    import { CatalogWorkflow } from './features/catalog/workflow.svelte';
    import { createConnectionActions } from './features/connection/actions';
    import { DeletionWorkflow } from './features/deletion/workflow.svelte';
    import { PickerController, type PickerRequest } from './features/dialogs/picker';
    import AppDialogs from './features/dialogs/AppDialogs.svelte';
    import { ExportWorkflow } from './features/export/workflow.svelte';
    import { AudioImportWorkflow, audioExtensions } from './features/import/audioWorkflow.svelte';
    import { PackageImportWorkflow } from './features/import/packageWorkflow.svelte';
    import { SequenceImportWorkflow } from './features/import/sequenceWorkflow.svelte';
    import { ImageSessionWorkflow } from './features/image-session/workflow.svelte';
    import { JobController } from './features/jobs/actions';
    import { MutationWorkflow } from './features/mutation/workflow.svelte';
    import WorkspaceShell from './features/workspace/WorkspaceShell.svelte';
    import { createTransport } from './lib/createTransport';
    import type { RemoteServerSettingsInput, RemoteServerSettingsView } from './lib/serverSettings';
    import { diagnosticsEnabled, reportDiagnostic } from './lib/diagnostics';
    import {
        emptyPackageExportSelection,
        maximumPackageExportRoots,
        type PackageExportSelectionState,
    } from './lib/objectSelection';
    import { userFacingMessage } from './lib/userFacingMessage';
    import type { InterfaceScaleController } from './lib/interfaceScale';
    import { midiExtensions } from './lib/midiImport';
    import type {
        DiskTreeItem,
        InspectorSelection,
        PackageExportObject,
        PackageExportSelection,
        ImageTreeAction,
        WorkspaceView,
    } from './lib/types';

    interface Props {
        interfaceScaling?: InterfaceScaleController | null;
    }

    let { interfaceScaling = null }: Props = $props();

    const workspaceTabs: {
        id: WorkspaceView;
        label: string;
        icon: 'music' | 'layers' | 'archive' | 'waveform' | 'list';
    }[] = [
        { id: 'programs', label: 'Programs', icon: 'music' },
        { id: 'sample-banks', label: 'Sample Banks', icon: 'layers' },
        { id: 'samples', label: 'Samples', icon: 'archive' },
        { id: 'wave-data', label: 'Wave Data', icon: 'waveform' },
        { id: 'sequences', label: 'Sequences', icon: 'list' },
    ];
    const transport = createTransport();
    const isDesktop = '__TAURI_INTERNALS__' in window;
    let pickerRequest = $state<PickerRequest | null>(null);
    let workspaceView = $state<WorkspaceView>('programs');
    let inspectorOpen = $state(true);
    let connectionSettings = $state<RemoteServerSettingsView | null>(null);
    let workspaceManagerOpen = $state(false);
    let audioFileInput = $state<HTMLInputElement>();
    let sequenceFileInput = $state<HTMLInputElement>();
    let packageExportSelection = $state<PackageExportSelectionState>(emptyPackageExportSelection());
    const connectionActions = createConnectionActions();
    const jobController = new JobController(transport);
    const pickerController = new PickerController((request) => {
        pickerRequest = request;
    });
    const imageSessionWorkflow = new ImageSessionWorkflow(transport, pickerController);
    const exportWorkflow = new ExportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        requestCompanionDisks: (retry) => imageSessionWorkflow.requestCompanionDisks(retry),
    });
    const catalogHooks = {
        stopPlayback: () => Promise.resolve(),
        resetPreviews: () => {},
        resetCleanup: () => {},
    };
    const catalog = new CatalogWorkflow({
        transport,
        sessionId: () => imageSessionWorkflow.sessionId,
        stopPlayback: () => catalogHooks.stopPlayback(),
        resetPreviews: () => catalogHooks.resetPreviews(),
        resetCleanup: () => catalogHooks.resetCleanup(),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const auditionWorkflow = new AuditionWorkflow({
        transport,
        catalog,
        sessionId: () => imageSessionWorkflow.sessionId,
        workspaceView: () => workspaceView,
        setWorkspaceView: (view) => (workspaceView = view),
        setInspectorOpen: (open) => (inspectorOpen = open),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        requestCompanionDisks: (retry) => imageSessionWorkflow.requestCompanionDisks(retry),
    });
    catalogHooks.stopPlayback = () => auditionWorkflow.stop();
    catalogHooks.resetPreviews = () => auditionWorkflow.resetPreviewQueue();
    const mutationWorkflow = new MutationWorkflow({
        transport,
        jobs: jobController,
        catalog,
        audition: auditionWorkflow,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageOpen: () => imageSessionWorkflow.location !== null,
        workspaceView: () => workspaceView,
        setWorkspaceView: (view) => (workspaceView = view),
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    const packageImportWorkflow = new PackageImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const deletionWorkflow = new DeletionWorkflow({
        transport,
        jobs: jobController,
        sessionId: () => imageSessionWorkflow.sessionId,
        activeVolumeId: () => catalog.activeVolumeId,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        stopPlayback: () => auditionWorkflow.stop(),
        selection: () => packageExportSelection,
        setSelection: (selection) => (packageExportSelection = selection),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    catalogHooks.resetCleanup = () => deletionWorkflow.resetCleanup();
    const audioImportWorkflow = new AudioImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        mutationsAvailable: () => mutationWorkflow.volumeAvailable,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        setSelectedSource: (item) => (imageSessionWorkflow.selectedSource = item),
        sourceItems: () => imageSessionWorkflow.sourceItems,
        activeVolumeId: () => catalog.activeVolumeId,
        samples: () => catalog.samples,
        loadVolume: (volumeId) => catalog.loadVolume(volumeId),
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        selectWorkspace: (view) => auditionWorkflow.selectWorkspaceView(view),
        selectSample: (sample) => void auditionWorkflow.selectSample(sample),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    const sequenceImportWorkflow = new SequenceImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        mutationsAvailable: () => mutationWorkflow.objectRenameAvailable,
        selectedVolume: () => {
            const selected = imageSessionWorkflow.selectedSource;
            return selected.kind === 'volume' && selected.partitionIndex !== undefined
                ? { partitionIndex: selected.partitionIndex, volumeName: selected.name }
                : null;
        },
        sequences: () => catalog.sequences,
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        selectWorkspace: (view) => auditionWorkflow.selectWorkspaceView(view),
        selectSequence: (sequence) => {
            catalog.selectedSequenceId = sequence.objectId;
            catalog.inspectorObjectId = sequence.objectId;
            catalog.editorObjectIds.sequences = sequence.objectId;
        },
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    imageSessionWorkflow.connect({
        catalog,
        audition: auditionWorkflow,
        mutation: mutationWorkflow,
        exports: exportWorkflow,
        packageImport: packageImportWorkflow,
        deletion: deletionWorkflow,
        clearExportSelection: clearPackageExportSelection,
    });
    const programs = $derived(catalog.programs);
    const sampleBanks = $derived(catalog.sampleBanks);
    const samples = $derived(catalog.samples);
    const waveData = $derived(catalog.waveData);
    const sequences = $derived(catalog.sequences);

    onDestroy(() => {
        exportWorkflow.dispose();
        deletionWorkflow.dispose();
        pickerController.dispose();
        void packageImportWorkflow.dispose();
        void jobController.dispose();
        void imageSessionWorkflow
            .dispose()
            .catch(() => undefined)
            .finally(() => auditionWorkflow.dispose().catch(() => undefined));
    });

    onMount(() => {
        return audioImportWorkflow.mountNativeDrops();
    });

    const selectedProgram = $derived(programs.find((item) => item.objectId === catalog.selectedProgramId));
    const selectedBank = $derived(sampleBanks.find((item) => item.objectId === catalog.selectedBankId));
    const selectedSample = $derived(samples.find((item) => item.objectId === catalog.selectedSampleId));
    const auditionableSampleObjectIds = $derived(auditionWorkflow.auditionableSampleObjectIds);
    const auditionableSampleBankObjectIds = $derived(auditionWorkflow.auditionableSampleBankObjectIds);
    const bankMembers = $derived(selectedBank ? catalog.membersForBank(selectedBank.objectId) : []);
    const bankMemberWaveData = $derived(
        catalog.selectedBankMemberId ? catalog.waveDataForSample(catalog.selectedBankMemberId) : [],
    );
    const sampleWaveData = $derived(selectedSample ? catalog.waveDataForSample(selectedSample.objectId) : []);
    const activeCollectionObjectId = $derived(
        workspaceView === 'programs'
            ? catalog.selectedProgramId
            : workspaceView === 'sample-banks'
              ? catalog.selectedBankId
              : workspaceView === 'samples'
                ? catalog.selectedSampleId
                : workspaceView === 'wave-data'
                  ? catalog.selectedWaveDataId
                  : catalog.selectedSequenceId,
    );
    const inspectorSelection = $derived.by<InspectorSelection>(() =>
        catalog.selectionForObject(catalog.inspectorObjectId, auditionWorkflow.sampleBankPreviewMemberId),
    );
    const editorSelection = $derived.by<InspectorSelection>(() =>
        catalog.selectionForObject(catalog.editorObjectIds[workspaceView], auditionWorkflow.sampleBankPreviewMemberId),
    );

    $effect(() => {
        const preview =
            inspectorSelection?.kind === 'sample'
                ? inspectorSelection.preview
                : inspectorSelection?.kind === 'sample-bank'
                  ? inspectorSelection.memberPreviews.find(
                        (member) => member.item.objectId === inspectorSelection.displayedMemberId,
                    )
                  : null;
        if (preview && preview.waveData.length > 0) auditionWorkflow.requestSampleWaveformPreview(preview.item);
    });

    $effect(() => {
        const sessionId = imageSessionWorkflow.sessionId;
        const objectId =
            inspectorSelection?.kind === 'sample'
                ? inspectorSelection.item.objectId
                : inspectorSelection?.kind === 'sample-bank'
                  ? inspectorSelection.displayedMemberId
                  : inspectorSelection?.kind === 'wave-data'
                    ? inspectorSelection.waveData.objectKey
                    : null;
        if (sessionId !== null && objectId && auditionWorkflow.auditionableObjectIds.has(objectId)) {
            auditionWorkflow.prefetchObject(objectId);
        }
    });

    function requestImageAction(item: DiskTreeItem, action: ImageTreeAction): void {
        if (item.partitionIndex === undefined) return;
        if (action === 'import-package') {
            if (!imageSessionWorkflow.packageImportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            packageImportWorkflow.open(item);
            return;
        }
        if (action === 'export-package') {
            if (!imageSessionWorkflow.packageExportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            exportWorkflow.requestPackage([
                {
                    kind: 'VOLUME',
                    partitionIndex: item.partitionIndex!,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (action === 'export-sfz') {
            if (!imageSessionWorkflow.audioExportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            void exportWorkflow.requestAudio([
                {
                    kind: 'VOLUME',
                    partitionIndex: item.partitionIndex,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (mutationWorkflow.requestVolumeAction(item, action)) imageSessionWorkflow.selectedSource = item;
    }

    function requestObjectPackageExport(items: PackageExportObject[]): void {
        if (
            !imageSessionWorkflow.packageExportAvailable ||
            items.length === 0 ||
            items.length > maximumPackageExportRoots
        ) {
            return;
        }
        exportWorkflow.requestPackage(items);
    }

    function clearPackageExportSelection(): void {
        packageExportSelection = emptyPackageExportSelection();
    }

    function reportPackageExportSelectionLimit(): void {
        imageSessionWorkflow.setStatus(
            `Package export supports at most ${maximumPackageExportRoots.toLocaleString()} selected objects`,
        );
    }

    async function requestAudioExport(items: PackageExportSelection[]): Promise<void> {
        if (!imageSessionWorkflow.audioExportAvailable) return;
        await exportWorkflow.requestAudio(items);
    }

    function requestSequenceExport(items: PackageExportObject[]): void {
        if (!imageSessionWorkflow.sequenceExportAvailable) return;
        exportWorkflow.requestSequence(items);
    }

    function requestObjectDeletion(targets: PackageExportObject[]): void {
        if (!imageSessionWorkflow.objectDeletionAvailable) return;
        deletionWorkflow.requestObjects(targets);
    }

    function requestWaveDataCleanup(): void {
        if (!imageSessionWorkflow.waveDataCleanupAvailable) return;
        deletionWorkflow.requestCleanup();
    }

    function reportMutationTiming(operation: string, started: number, itemCount: number): void {
        if (!diagnosticsEnabled()) return;
        reportDiagnostic('image_mutation_completed', {
            operation,
            itemCount,
            durationMs: Math.round(performance.now() - started),
            strategy: 'journaled-in-place',
        });
    }

    function suppressDesktopContextMenu(event: MouseEvent): void {
        if (isDesktop) event.preventDefault();
    }

    async function openConnectionSettings(): Promise<void> {
        try {
            connectionSettings = await connectionActions.load();
        } catch (error) {
            imageSessionWorkflow.setStatus(userFacingMessage(error));
        }
    }

    async function saveRemoteConnection(input: RemoteServerSettingsInput): Promise<void> {
        await connectionActions.saveRemote(input);
    }

    async function switchToLocalConnection(): Promise<void> {
        await connectionActions.useLocal();
    }
</script>

<svelte:head><title>axkdeck · A-series disk workspace</title></svelte:head>
<svelte:window
    oncontextmenu={suppressDesktopContextMenu}
    ondragenter={(event) => audioImportWorkflow.drag(event)}
    ondragover={(event) => audioImportWorkflow.drag(event)}
    ondragleave={(event) => audioImportWorkflow.leave(event)}
    ondrop={(event) => audioImportWorkflow.drop(event)}
/>

<input
    bind:this={audioFileInput}
    class="sr-only"
    type="file"
    multiple
    accept={audioExtensions.map((extension) => `.${extension}`).join(',')}
    onchange={(event) => audioImportWorkflow.filesChosen(event)}
/>
<input
    bind:this={sequenceFileInput}
    class="sr-only"
    type="file"
    multiple
    accept={midiExtensions.map((extension) => `.${extension}`).join(',')}
    onchange={(event) => sequenceImportWorkflow.filesChosen(event)}
/>

<WorkspaceShell
    {transport}
    {interfaceScaling}
    {isDesktop}
    {workspaceTabs}
    {workspaceView}
    bind:inspectorOpen
    imageLocation={imageSessionWorkflow.location}
    sourceItems={imageSessionWorkflow.sourceItems}
    selectedSource={imageSessionWorkflow.selectedSource}
    imageOpening={imageSessionWorkflow.opening}
    sessionId={imageSessionWorkflow.sessionId}
    {catalog}
    audition={auditionWorkflow}
    mutation={mutationWorkflow}
    audioImport={audioImportWorkflow}
    sequenceImport={sequenceImportWorkflow}
    {programs}
    {sampleBanks}
    {samples}
    {waveData}
    {sequences}
    {bankMembers}
    {bankMemberWaveData}
    {sampleWaveData}
    {activeCollectionObjectId}
    {inspectorSelection}
    {editorSelection}
    sourceStatus={imageSessionWorkflow.status}
    packageSelection={packageExportSelection}
    objectDeletionAvailable={imageSessionWorkflow.objectDeletionAvailable}
    waveDataCleanupAvailable={imageSessionWorkflow.waveDataCleanupAvailable}
    packageImportAvailable={imageSessionWorkflow.packageImportAvailable}
    packageExportAvailable={imageSessionWorkflow.packageExportAvailable}
    audioExportAvailable={imageSessionWorkflow.audioExportAvailable}
    sequenceExportAvailable={imageSessionWorkflow.sequenceExportAvailable}
    openConnectionSettings={() => void openConnectionSettings()}
    openImage={() => void imageSessionWorkflow.chooseAndOpen()}
    createImage={() => void imageSessionWorkflow.chooseHardDiskDirectory()}
    closeImage={() => void imageSessionWorkflow.close().catch(() => undefined)}
    manageLocations={() => (workspaceManagerOpen = true)}
    selectSource={(item) => imageSessionWorkflow.selectSource(item)}
    imageAction={requestImageAction}
    selectWorkspace={(view) => auditionWorkflow.selectWorkspaceView(view)}
    exportPackage={requestObjectPackageExport}
    exportAudio={(items) => void requestAudioExport(items)}
    exportMidi={requestSequenceExport}
    deleteObjects={requestObjectDeletion}
    cleanupWaveData={requestWaveDataCleanup}
    clearSelection={clearPackageExportSelection}
    selectionChanged={(selection) => (packageExportSelection = selection)}
    selectionLimit={reportPackageExportSelectionLimit}
/>

<AppDialogs
    {transport}
    {isDesktop}
    {pickerRequest}
    finishPicker={(selection) => pickerController.finish(selection)}
    manageLocations={() => (workspaceManagerOpen = true)}
    companionRequest={imageSessionWorkflow.companionRequest}
    addCompanion={() => void imageSessionWorkflow.addCompanionDiskFolder()}
    removeCompanion={(directory) => imageSessionWorkflow.removeCompanionDiskFolder(directory)}
    attachCompanions={(selection) => void imageSessionWorkflow.attachCompanionDisks(selection)}
    cancelCompanions={() => imageSessionWorkflow.cancelCompanionDisks()}
    hardDiskDirectory={imageSessionWorkflow.hardDiskDirectory}
    finishHardDisk={(file) => imageSessionWorkflow.finishHardDiskCreation(file)}
    cancelHardDisk={() => imageSessionWorkflow.cancelHardDiskCreation()}
    {workspaceManagerOpen}
    activeWorkspaceId={imageSessionWorkflow.location?.reference.rootId ?? null}
    closeWorkspaceManager={() => (workspaceManagerOpen = false)}
    {connectionSettings}
    {saveRemoteConnection}
    {switchToLocalConnection}
    closeConnectionSettings={() => (connectionSettings = null)}
    mutation={mutationWorkflow}
    packageImport={packageImportWorkflow}
    exports={exportWorkflow}
    deletion={deletionWorkflow}
    audioImport={audioImportWorkflow}
    {audioFileInput}
    sequenceImport={sequenceImportWorkflow}
    {sequenceFileInput}
    sampleNames={samples.map((item) => item.name)}
    waveDataNames={waveData.map((item) => item.name)}
    sequenceNames={sequences.map((item) => item.name)}
/>
