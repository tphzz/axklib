<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import type { AppProps } from './appProps';
    import { AuditionWorkflow } from './features/audition/workflow.svelte';
    import { createCatalogHooks } from './features/catalog/hooks';
    import { CatalogWorkflow } from './features/catalog/workflow.svelte';
    import { createConnectionActions } from './features/connection/actions';
    import { DeletionWorkflow } from './features/deletion/workflow.svelte';
    import { PickerController, type PickerRequest } from './features/dialogs/picker';
    import PickerDialogHost from './features/dialogs/PickerDialogHost.svelte';
    import { hasOpenAppDialog } from './features/dialogs/visibility';
    import ClientFileInputs from './features/file-operations/ClientFileInputs.svelte';
    import { DirectComputerWorkflow } from './features/file-operations/directComputerWorkflow';
    import { ExportWorkflow } from './features/export/workflow.svelte';
    import { MediaExportWorkflow } from './features/export/mediaWorkflow.svelte';
    import { VolumePackageExportWorkflow } from './features/export/volumePackageWorkflow.svelte';
    import { VolumeFloppyExportWorkflow } from './features/export/volumeFloppyWorkflow.svelte';
    import { AudioImportWorkflow } from './features/import/audioWorkflow.svelte';
    import { MediaDropWorkflow } from './features/import/mediaDropWorkflow.svelte';
    import { PackageImportWorkflow } from './features/import/packageWorkflow.svelte';
    import { PackageBatchImportWorkflow } from './features/import/packageBatchWorkflow.svelte';
    import { PackagePickerHistory } from './features/import/packagePickerHistory';
    import { SequenceImportWorkflow } from './features/import/sequenceWorkflow.svelte';
    import { Tx16wImportWorkflow } from './features/import/tx16wWorkflow.svelte';
    import { ImageSessionWorkflow } from './features/image-session/workflow.svelte';
    import { createImageTreeActionHandler } from './features/image-session/treeActions';
    import { ExtentLayoutRepairWorkflow } from './features/image-session/extentLayoutRepairWorkflow.svelte';
    import { JobController } from './features/jobs/actions';
    import { MutationWorkflow } from './features/mutation/workflow.svelte';
    import { ProgramGenerationWorkflow } from './features/program-generation/workflow.svelte';
    import { ProgramAssignmentCleanupWorkflow } from './features/program-assignment-cleanup/workflow.svelte';
    import WorkspaceShell from './features/workspace/WorkspaceShell.svelte';
    import { workspaceTabs } from './features/workspace/tabs';
    import ExperimentalWarningDialog from './lib/components/ExperimentalWarningDialog.svelte';
    import ImageIntegrityDialog from './lib/components/ImageIntegrityDialog.svelte';
    import ImageOpenProgressDialog from './lib/components/ImageOpenProgressDialog.svelte';
    import WorkspaceGuard from './lib/components/WorkspaceGuard.svelte';
    import { createTransport } from './lib/createTransport';
    import type { RemoteServerSettingsInput, RemoteServerSettingsView } from './lib/serverSettings';
    import { reportMutationTiming } from './lib/diagnostics';
    import {
        emptyPackageExportSelection,
        maximumPackageExportRoots,
        type PackageExportSelectionState,
    } from './lib/objectSelection';
    import { userFacingMessage } from './lib/userFacingMessage';
    import type { InspectorSelection, PackageExportObject, PackageExportSelection, WorkspaceView } from './lib/types';

    let {
        interfaceScaling = null,
        initialExperimentalWarningOpen = true,
        openConnectionSettingsOnStart = false,
    }: AppProps = $props();
    const transport = createTransport();
    const isDesktop = '__TAURI_INTERNALS__' in window;
    let pickerRequest = $state<PickerRequest | null>(null);
    let experimentalWarningAcknowledged = $state(false);
    const experimentalWarningOpen = $derived(initialExperimentalWarningOpen && !experimentalWarningAcknowledged);
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
    const packagePickerHistory = new PackagePickerHistory();
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
    const mediaExportWorkflow = new MediaExportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const extentLayoutRepairWorkflow = new ExtentLayoutRepairWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        closeDialog: () => imageSessionWorkflow.closeIntegrity(),
    });
    const volumePackageExportWorkflow = new VolumePackageExportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const volumeFloppyExportWorkflow = new VolumeFloppyExportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const catalogHooks = createCatalogHooks();
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
        clearSelection: () => (packageExportSelection = emptyPackageExportSelection()),
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
        pickerHistory: packagePickerHistory,
        mutationsAvailable: () => imageSessionWorkflow.packageImportAvailable,
        selectedSource: () => imageSessionWorkflow.importDestinationSource(),
        sourceItems: () => imageSessionWorkflow.sourceItems,
    });
    const packageBatchImportWorkflow = new PackageBatchImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        pickerHistory: packagePickerHistory,
        isDesktop,
        sessionId: () => imageSessionWorkflow.sessionId,
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        mutationsAvailable: () => imageSessionWorkflow.packageImportAvailable,
        sourceItems: () => imageSessionWorkflow.sourceItems,
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
    const programGenerationWorkflow = new ProgramGenerationWorkflow({
        transport,
        jobs: jobController,
        sessionId: () => imageSessionWorkflow.sessionId,
        activeVolumeId: () => catalog.activeVolumeId,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        selectWorkspace: (view) => auditionWorkflow.selectWorkspaceView(view),
        selectProgram: (programNumber) => {
            const program = catalog.programs.find((candidate) => Number(candidate.slot) === programNumber);
            if (program) auditionWorkflow.selectProgram(program);
        },
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    const programAssignmentCleanupWorkflow = new ProgramAssignmentCleanupWorkflow({
        transport,
        jobs: jobController,
        sessionId: () => imageSessionWorkflow.sessionId,
        activeVolumeId: () => catalog.activeVolumeId,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    catalogHooks.resetCleanup = () => deletionWorkflow.resetCleanup();
    const audioImportWorkflow = new AudioImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        imageFormat: () => imageSessionWorkflow.imageFormat,
        mutationsAvailable: () => mutationWorkflow.volumeAvailable,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        setSelectedSource: (item) => imageSessionWorkflow.selectSource(item),
        sourceItems: () => imageSessionWorkflow.sourceItems,
        activeVolumeId: () => catalog.activeVolumeId,
        sampleBanks: () => catalog.sampleBanks,
        samples: () => catalog.samples,
        loadVolume: (volumeId) => catalog.loadVolume(volumeId),
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        selectWorkspace: (view) => auditionWorkflow.selectWorkspaceView(view),
        selectSampleBank: (sampleBank) => void auditionWorkflow.selectBank(sampleBank),
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
        imageFormat: () => imageSessionWorkflow.imageFormat,
        mutationsAvailable: () => mutationWorkflow.volumeAvailable,
        selectedSource: () => imageSessionWorkflow.importDestinationSource(),
        setSelectedSource: (item) => imageSessionWorkflow.selectSource(item),
        sourceItems: () => imageSessionWorkflow.sourceItems,
        activeVolumeId: () => catalog.activeVolumeId,
        sequences: () => catalog.sequences,
        loadVolume: (volumeId) => catalog.loadVolume(volumeId),
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
    const tx16wImportWorkflow = new Tx16wImportWorkflow({
        transport,
        jobs: jobController,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        mutationsAvailable: () => imageSessionWorkflow.packageImportAvailable,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        sourceItems: () => imageSessionWorkflow.sourceItems,
        refreshSession: (preferred) => imageSessionWorkflow.refresh(preferred),
        invalidateSession: (sessionId) => auditionWorkflow.invalidateSession(sessionId),
        selectWorkspace: (view) => auditionWorkflow.selectWorkspaceView(view),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
        reportTiming: reportMutationTiming,
    });
    const directComputerWorkflow = new DirectComputerWorkflow(isDesktop, transport.connectionMode);
    const pendingDirectComputerOperation = directComputerWorkflow.pendingOperation;
    const mediaDropWorkflow = new MediaDropWorkflow({
        isDesktop,
        workspaceView: () => workspaceView,
        audioImport: audioImportWorkflow,
        sequenceImport: sequenceImportWorkflow,
        tx16wImport: tx16wImportWorkflow,
        packageImport: packageImportWorkflow,
        packageBatchImport: packageBatchImportWorkflow,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageFormat: () => imageSessionWorkflow.imageFormat,
        mutationsAvailable: () => mutationWorkflow.volumeAvailable,
        selectedSource: () => imageSessionWorkflow.importDestinationSource(),
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
    });
    const requestImageAction = createImageTreeActionHandler({
        transport,
        imageSession: imageSessionWorkflow,
        mutation: mutationWorkflow,
        directComputer: directComputerWorkflow,
        packageBatchImport: packageBatchImportWorkflow,
        exports: exportWorkflow,
        volumePackages: volumePackageExportWorkflow,
        volumeFloppies: volumeFloppyExportWorkflow,
        mediaExports: mediaExportWorkflow,
        isDesktop,
        exportAudio: requestAudioExport,
    });
    imageSessionWorkflow.connect({
        catalog,
        audition: auditionWorkflow,
        mutation: mutationWorkflow,
        exports: exportWorkflow,
        volumePackages: volumePackageExportWorkflow,
        volumeFloppies: volumeFloppyExportWorkflow,
        mediaExports: mediaExportWorkflow,
        packageImport: packageImportWorkflow,
        deletion: deletionWorkflow,
        programGeneration: programGenerationWorkflow,
        extentRepairs: extentLayoutRepairWorkflow,
        clearExportSelection: clearPackageExportSelection,
    });
    const programs = $derived(catalog.programs);
    const sampleBanks = $derived(catalog.sampleBanks);
    const samples = $derived(catalog.samples);
    const waveData = $derived(catalog.waveData);
    const sequences = $derived(catalog.sequences);

    onDestroy(() => {
        exportWorkflow.dispose();
        volumePackageExportWorkflow.dispose();
        volumeFloppyExportWorkflow.dispose();
        mediaExportWorkflow.dispose();
        extentLayoutRepairWorkflow.dispose();
        deletionWorkflow.dispose();
        programGenerationWorkflow.dispose();
        programAssignmentCleanupWorkflow.dispose();
        pickerController.dispose();
        void packageImportWorkflow.dispose();
        void packageBatchImportWorkflow.close();
        void tx16wImportWorkflow.close();
        void jobController.dispose();
        void imageSessionWorkflow
            .dispose()
            .catch(() => undefined)
            .finally(() => auditionWorkflow.dispose().catch(() => undefined));
    });

    onMount(() => {
        const unmountNativeDrops = mediaDropWorkflow.mountNativeDrops();
        if (openConnectionSettingsOnStart) void openConnectionSettings();
        return unmountNativeDrops;
    });

    const selectedProgram = $derived(programs.find((item) => item.objectId === catalog.selectedProgramId));
    const activeWorkspaceId = $derived(imageSessionWorkflow.location?.reference.rootId ?? null);
    const selectedBank = $derived(sampleBanks.find((item) => item.objectId === catalog.selectedBankId));
    const selectedSample = $derived(samples.find((item) => item.objectId === catalog.selectedSampleId));
    const auditionableSampleObjectIds = $derived(auditionWorkflow.auditionableSampleObjectIds);
    const auditionableSampleBankObjectIds = $derived(auditionWorkflow.auditionableSampleBankObjectIds);
    const appDialogsOpen = $derived(
        hasOpenAppDialog({
            pickerRequest,
            imageSession: imageSessionWorkflow,
            connectionSettings,
            mutation: mutationWorkflow,
            packageImport: packageImportWorkflow,
            packageBatchImport: packageBatchImportWorkflow,
            exports: exportWorkflow,
            volumePackages: volumePackageExportWorkflow,
            volumeFloppies: volumeFloppyExportWorkflow,
            mediaExports: mediaExportWorkflow,
            deletion: deletionWorkflow,
            programGeneration: programGenerationWorkflow,
            programAssignmentCleanup: programAssignmentCleanupWorkflow,
            audioImport: audioImportWorkflow,
            sequenceImport: sequenceImportWorkflow,
            tx16wImport: tx16wImportWorkflow,
            mediaDrop: mediaDropWorkflow,
        }),
    );
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

    function requestObjectPackageExport(items: PackageExportObject[]): void {
        if (
            !imageSessionWorkflow.packageExportAvailable ||
            items.length === 0 ||
            items.length > maximumPackageExportRoots
        ) {
            return;
        }
        directComputerWorkflow.exportPackage(exportWorkflow, items);
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
        await directComputerWorkflow.exportAudio(exportWorkflow, items);
    }
    async function requestWavExport(items: PackageExportSelection[]): Promise<void> {
        if (!imageSessionWorkflow.audioExportAvailable) return;
        await directComputerWorkflow.exportWav(exportWorkflow, items);
    }
    function requestSequenceExport(items: PackageExportObject[]): void {
        if (!imageSessionWorkflow.sequenceExportAvailable) return;
        directComputerWorkflow.exportMidi(exportWorkflow, items);
    }

    function requestObjectDeletion(targets: PackageExportObject[]): void {
        if (!imageSessionWorkflow.objectDeletionAvailable) return;
        deletionWorkflow.requestObjects(targets);
    }
    function requestWaveDataCleanup(): void {
        if (!imageSessionWorkflow.waveDataCleanupAvailable) return;
        deletionWorkflow.requestCleanup();
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
    ondragenter={(event) => mediaDropWorkflow.drag(event)}
    ondragover={(event) => mediaDropWorkflow.drag(event)}
    ondragleave={(event) => mediaDropWorkflow.leave(event)}
    ondrop={(event) => mediaDropWorkflow.drop(event)}
/>

<ClientFileInputs
    bind:audioInput={audioFileInput}
    bind:sequenceInput={sequenceFileInput}
    audioChanged={(event) => directComputerWorkflow.audioFilesChosen(audioImportWorkflow, event)}
    audioCancelled={() => directComputerWorkflow.cancelAudioSelection(audioImportWorkflow)}
    sequenceChanged={(event) => directComputerWorkflow.midiFilesChosen(sequenceImportWorkflow, event)}
    sequenceCancelled={() => directComputerWorkflow.cancelMidiSelection(sequenceImportWorkflow)}
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
    selectedVolumeIds={imageSessionWorkflow.volumeSelection.items.map((item) => item.id)}
    imageOpening={imageSessionWorkflow.opening}
    sessionId={imageSessionWorkflow.sessionId}
    {catalog}
    audition={auditionWorkflow}
    mutation={mutationWorkflow}
    audioImport={audioImportWorkflow}
    sequenceImport={sequenceImportWorkflow}
    importAudio={() => directComputerWorkflow.importAudio(audioImportWorkflow, audioFileInput)}
    importMidi={() => directComputerWorkflow.importMidi(sequenceImportWorkflow, sequenceFileInput)}
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
    programGenerationAvailable={imageSessionWorkflow.programGenerationAvailable}
    programAssignmentCleanupAvailable={imageSessionWorkflow.programAssignmentCleanupAvailable}
    packageImportAvailable={imageSessionWorkflow.packageImportAvailable}
    packageExportAvailable={imageSessionWorkflow.packageExportAvailable}
    volumePackageExportAvailable={imageSessionWorkflow.volumePackageExportAvailable}
    volumeFloppyExportAvailable={imageSessionWorkflow.volumeFloppyExportAvailable}
    audioExportAvailable={imageSessionWorkflow.audioExportAvailable}
    sequenceExportAvailable={imageSessionWorkflow.sequenceExportAvailable}
    mediaConversionAvailable={imageSessionWorkflow.mediaConversionAvailable}
    allocationInspectionAvailable={imageSessionWorkflow.allocationInspectionAvailable}
    samplerOrderingEnabled={imageSessionWorkflow.imageFormat === 'sfs'}
    openConnectionSettings={() => void openConnectionSettings()}
    openImage={() => void imageSessionWorkflow.chooseAndOpen()}
    createImage={() => void imageSessionWorkflow.chooseHardDiskDirectory()}
    closeImage={() => void imageSessionWorkflow.close().catch(() => undefined)}
    showImageIntegrity={() => void imageSessionWorkflow.showIntegrity()}
    manageLocations={() => (workspaceManagerOpen = true)}
    selectSource={(item, mode, visibleVolumes) => imageSessionWorkflow.selectTreeSource(item, mode, visibleVolumes)}
    selectSourceForContext={(item, visibleVolumes) => imageSessionWorkflow.selectSourceForContext(item, visibleVolumes)}
    imageAction={requestImageAction}
    selectWorkspace={(view) => auditionWorkflow.selectWorkspaceView(view)}
    exportPackage={requestObjectPackageExport}
    exportAudio={(items) => void requestAudioExport(items)}
    exportWav={(items) => void requestWavExport(items)}
    exportMidi={requestSequenceExport}
    deleteObjects={requestObjectDeletion}
    cleanupWaveData={requestWaveDataCleanup}
    generatePrograms={() => void programGenerationWorkflow.open()}
    cleanupProgramAssignments={() => void programAssignmentCleanupWorkflow.open()}
    clearSelection={clearPackageExportSelection}
    selectionChanged={(selection) => (packageExportSelection = selection)}
    selectionLimit={reportPackageExportSelectionLimit}
    setStatus={(status) => imageSessionWorkflow.setStatus(status)}
/>

{#if experimentalWarningOpen}
    <ExperimentalWarningDialog onacknowledge={() => (experimentalWarningAcknowledged = true)} />
{/if}

{#if !experimentalWarningOpen && imageSessionWorkflow.integrityDialogOpen}
    <ImageIntegrityDialog
        issues={imageSessionWorkflow.integrityIssues}
        loading={imageSessionWorkflow.integrityLoading}
        error={imageSessionWorkflow.integrityError}
        repairAvailable={imageSessionWorkflow.extentLayoutRepairAvailable}
        repairing={extentLayoutRepairWorkflow.busy}
        repairLabel={extentLayoutRepairWorkflow.progressLabel}
        repairError={extentLayoutRepairWorkflow.error}
        onrepair={() => void extentLayoutRepairWorkflow.repair()}
        onclose={() => imageSessionWorkflow.closeIntegrity()}
    />
{/if}

<WorkspaceGuard enabled={!experimentalWarningOpen} bind:open={workspaceManagerOpen} {activeWorkspaceId} />

{#if !experimentalWarningOpen}
    <PickerDialogHost
        {transport}
        request={pickerRequest}
        finish={(selection) => pickerController.finish(selection)}
        manageLocations={() => (workspaceManagerOpen = true)}
    />
{/if}

{#if !experimentalWarningOpen && imageSessionWorkflow.openProgressVisible}
    <ImageOpenProgressDialog
        label={imageSessionWorkflow.openProgressLabel}
        completed={imageSessionWorkflow.openProgressCompleted}
        total={imageSessionWorkflow.openProgressTotal}
        cancellable={imageSessionWorkflow.openProgressCancellable}
        cancelling={imageSessionWorkflow.openProgressCancelling}
        oncancel={() => imageSessionWorkflow.cancelOpen()}
    />
{/if}

{#if !experimentalWarningOpen && appDialogsOpen}
    {#await import('./features/dialogs/AppDialogs.svelte') then dialogs}
        {@const AppDialogs = dialogs.default}
        <AppDialogs
            {transport}
            {isDesktop}
            directComputerOperation={$pendingDirectComputerOperation}
            {pickerRequest}
            companionRequest={imageSessionWorkflow.companionRequest}
            addCompanion={() => void imageSessionWorkflow.addCompanionDiskSource()}
            removeCompanion={(source) => imageSessionWorkflow.removeCompanionDiskSource(source)}
            attachCompanions={(selection) => void imageSessionWorkflow.attachCompanionDisks(selection)}
            cancelCompanions={() => imageSessionWorkflow.cancelCompanionDisks()}
            hardDiskDirectory={imageSessionWorkflow.hardDiskDirectory}
            finishHardDisk={(file) => imageSessionWorkflow.finishHardDiskCreation(file)}
            cancelHardDisk={() => imageSessionWorkflow.cancelHardDiskCreation()}
            {connectionSettings}
            {saveRemoteConnection}
            {switchToLocalConnection}
            closeConnectionSettings={() => (connectionSettings = null)}
            mutation={mutationWorkflow}
            packageImport={packageImportWorkflow}
            packageBatchImport={packageBatchImportWorkflow}
            exports={exportWorkflow}
            volumePackages={volumePackageExportWorkflow}
            volumeFloppies={volumeFloppyExportWorkflow}
            mediaExports={mediaExportWorkflow}
            deletion={deletionWorkflow}
            programGeneration={programGenerationWorkflow}
            programAssignmentCleanup={programAssignmentCleanupWorkflow}
            mediaDrop={mediaDropWorkflow}
            tx16wImport={tx16wImportWorkflow}
            audioImport={audioImportWorkflow}
            {audioFileInput}
            sequenceImport={sequenceImportWorkflow}
            {sequenceFileInput}
            sampleNames={samples.map((item) => item.name)}
            sampleBankNames={sampleBanks.map((item) => item.name)}
            waveDataNames={waveData.map((item) => item.name)}
        />
    {/await}
{/if}
