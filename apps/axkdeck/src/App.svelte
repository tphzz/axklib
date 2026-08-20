<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import type { AppProps } from './appProps';
    import { AuditionWorkflow } from './features/audition/workflow.svelte';
    import { createCatalogHooks } from './features/catalog/hooks';
    import { CatalogWorkflow } from './features/catalog/workflow.svelte';
    import { createConnectionActions } from './features/connection/actions';
    import { DeletionWorkflow } from './features/deletion/workflow.svelte';
    import { PickerController, type PickerRequest } from './features/dialogs/picker';
    import { hasOpenAppDialog } from './features/dialogs/visibility';
    import { ExportWorkflow } from './features/export/workflow.svelte';
    import { MediaExportWorkflow } from './features/export/mediaWorkflow.svelte';
    import { VolumePackageExportWorkflow } from './features/export/volumePackageWorkflow.svelte';
    import { VolumeFloppyExportWorkflow } from './features/export/volumeFloppyWorkflow.svelte';
    import { AudioImportWorkflow, audioExtensions } from './features/import/audioWorkflow.svelte';
    import { MediaDropWorkflow } from './features/import/mediaDropWorkflow.svelte';
    import { PackageImportWorkflow } from './features/import/packageWorkflow.svelte';
    import { PackageBatchImportWorkflow } from './features/import/packageBatchWorkflow.svelte';
    import { PackagePickerHistory } from './features/import/packagePickerHistory';
    import { SequenceImportWorkflow } from './features/import/sequenceWorkflow.svelte';
    import { Tx16wImportWorkflow } from './features/import/tx16wWorkflow.svelte';
    import { ImageSessionWorkflow } from './features/image-session/workflow.svelte';
    import { ExtentLayoutRepairWorkflow } from './features/image-session/extentLayoutRepairWorkflow.svelte';
    import { JobController } from './features/jobs/actions';
    import { MutationWorkflow } from './features/mutation/workflow.svelte';
    import { ProgramGenerationWorkflow } from './features/program-generation/workflow.svelte';
    import WorkspaceShell from './features/workspace/WorkspaceShell.svelte';
    import { workspaceTabs } from './features/workspace/tabs';
    import ExperimentalWarningDialog from './lib/components/ExperimentalWarningDialog.svelte';
    import ImageIntegrityDialog from './lib/components/ImageIntegrityDialog.svelte';
    import { createTransport } from './lib/createTransport';
    import { openAllocationInspector } from './lib/allocationInspector';
    import type { RemoteServerSettingsInput, RemoteServerSettingsView } from './lib/serverSettings';
    import { reportMutationTiming } from './lib/diagnostics';
    import {
        emptyPackageExportSelection,
        maximumPackageExportRoots,
        type PackageExportSelectionState,
    } from './lib/objectSelection';
    import { userFacingMessage } from './lib/userFacingMessage';
    import { midiExtensions } from './lib/midiImport';
    import type {
        DiskTreeItem,
        InspectorSelection,
        PackageExportObject,
        PackageExportSelection,
        ImageTreeAction,
        WorkspaceView,
    } from './lib/types';

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
        selectedSource: () => imageSessionWorkflow.selectedSource,
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
    catalogHooks.resetCleanup = () => deletionWorkflow.resetCleanup();
    const audioImportWorkflow = new AudioImportWorkflow({
        transport,
        jobs: jobController,
        picker: pickerController,
        sessionId: () => imageSessionWorkflow.sessionId,
        imageLocation: () => imageSessionWorkflow.location,
        mutationsAvailable: () => mutationWorkflow.volumeAvailable,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        setSelectedSource: (item) => (imageSessionWorkflow.selectedSource = item),
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
        mutationsAvailable: () => mutationWorkflow.objectRenameAvailable,
        selectedSource: () => imageSessionWorkflow.selectedSource,
        setSelectedSource: (item) => (imageSessionWorkflow.selectedSource = item),
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
    const mediaDropWorkflow = new MediaDropWorkflow({
        isDesktop,
        workspaceView: () => workspaceView,
        audioImport: audioImportWorkflow,
        sequenceImport: sequenceImportWorkflow,
        tx16wImport: tx16wImportWorkflow,
        packageImport: packageImportWorkflow,
        setStatus: (status) => imageSessionWorkflow.setStatus(status),
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
    const selectedBank = $derived(sampleBanks.find((item) => item.objectId === catalog.selectedBankId));
    const selectedSample = $derived(samples.find((item) => item.objectId === catalog.selectedSampleId));
    const auditionableSampleObjectIds = $derived(auditionWorkflow.auditionableSampleObjectIds);
    const auditionableSampleBankObjectIds = $derived(auditionWorkflow.auditionableSampleBankObjectIds);
    const appDialogsOpen = $derived(
        hasOpenAppDialog({
            pickerRequest,
            imageSession: imageSessionWorkflow,
            workspaceManagerOpen,
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

    function requestImageAction(item: DiskTreeItem, action: ImageTreeAction): void {
        if (item.partitionIndex === undefined) return;
        if (action === 'inspect-allocation') {
            const sessionId = imageSessionWorkflow.sessionId;
            if (
                !isDesktop ||
                sessionId === null ||
                !imageSessionWorkflow.allocationInspectionAvailable ||
                item.kind !== 'partition'
            )
                return;
            imageSessionWorkflow.selectedSource = item;
            void transport
                .allocationMapReference(sessionId)
                .then((reference) =>
                    openAllocationInspector({
                        ...reference,
                        partitionIndex: item.partitionIndex!,
                        partitionName: item.name,
                    }),
                )
                .catch((error) => imageSessionWorkflow.setStatus(userFacingMessage(error)));
            return;
        }
        if (action === 'import-package') {
            if (!imageSessionWorkflow.packageImportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            packageImportWorkflow.open(item);
            return;
        }
        if (action === 'import-packages') {
            if (!imageSessionWorkflow.packageImportAvailable || item.kind !== 'partition') return;
            imageSessionWorkflow.selectedSource = item;
            packageBatchImportWorkflow.open(item);
            return;
        }
        if (action === 'export-package') {
            if (!imageSessionWorkflow.packageExportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            exportWorkflow.requestPackage([
                {
                    kind: 'VOLUME',
                    contentId: item.id,
                    partitionIndex: item.partitionIndex!,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (action === 'export-volume-packages') {
            if (!imageSessionWorkflow.volumePackageExportAvailable || item.kind !== 'partition') return;
            imageSessionWorkflow.selectedSource = item;
            void volumePackageExportWorkflow.open(item);
            return;
        }
        if (action === 'export-volume-floppies') {
            if (!imageSessionWorkflow.volumeFloppyExportAvailable || item.kind !== 'partition') return;
            imageSessionWorkflow.selectedSource = item;
            void volumeFloppyExportWorkflow.open(item);
            return;
        }
        if (action === 'export-sfz') {
            if (!imageSessionWorkflow.audioExportAvailable || item.kind !== 'volume') return;
            imageSessionWorkflow.selectedSource = item;
            void exportWorkflow.requestAudio([
                {
                    kind: 'VOLUME',
                    contentId: item.id,
                    partitionIndex: item.partitionIndex,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (action === 'export-cdrom' || action === 'export-floppy') {
            if (!imageSessionWorkflow.mediaConversionAvailable) return;
            imageSessionWorkflow.selectedSource = item;
            void mediaExportWorkflow.open(item);
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
    programGenerationAvailable={imageSessionWorkflow.programGenerationAvailable}
    packageImportAvailable={imageSessionWorkflow.packageImportAvailable}
    packageExportAvailable={imageSessionWorkflow.packageExportAvailable}
    volumePackageExportAvailable={imageSessionWorkflow.volumePackageExportAvailable}
    volumeFloppyExportAvailable={imageSessionWorkflow.volumeFloppyExportAvailable}
    audioExportAvailable={imageSessionWorkflow.audioExportAvailable}
    sequenceExportAvailable={imageSessionWorkflow.sequenceExportAvailable}
    mediaConversionAvailable={imageSessionWorkflow.mediaConversionAvailable}
    allocationInspectionAvailable={imageSessionWorkflow.allocationInspectionAvailable}
    openConnectionSettings={() => void openConnectionSettings()}
    openImage={() => void imageSessionWorkflow.chooseAndOpen()}
    createImage={() => void imageSessionWorkflow.chooseHardDiskDirectory()}
    closeImage={() => void imageSessionWorkflow.close().catch(() => undefined)}
    showImageIntegrity={() => void imageSessionWorkflow.showIntegrity()}
    manageLocations={() => (workspaceManagerOpen = true)}
    selectSource={(item) => imageSessionWorkflow.selectSource(item)}
    imageAction={requestImageAction}
    selectWorkspace={(view) => auditionWorkflow.selectWorkspaceView(view)}
    exportPackage={requestObjectPackageExport}
    exportAudio={(items) => void requestAudioExport(items)}
    exportMidi={requestSequenceExport}
    deleteObjects={requestObjectDeletion}
    cleanupWaveData={requestWaveDataCleanup}
    generatePrograms={() => void programGenerationWorkflow.open()}
    clearSelection={clearPackageExportSelection}
    selectionChanged={(selection) => (packageExportSelection = selection)}
    selectionLimit={reportPackageExportSelectionLimit}
/>

{#if experimentalWarningOpen}
    <ExperimentalWarningDialog onacknowledge={() => (experimentalWarningAcknowledged = true)} />
{/if}

{#if imageSessionWorkflow.integrityDialogOpen}
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

{#if appDialogsOpen}
    {#await import('./features/dialogs/AppDialogs.svelte') then dialogs}
        {@const AppDialogs = dialogs.default}
        <AppDialogs
            {transport}
            {isDesktop}
            {pickerRequest}
            finishPicker={(selection) => pickerController.finish(selection)}
            manageLocations={() => (workspaceManagerOpen = true)}
            companionRequest={imageSessionWorkflow.companionRequest}
            addCompanion={() => void imageSessionWorkflow.addCompanionDiskSource()}
            removeCompanion={(source) => imageSessionWorkflow.removeCompanionDiskSource(source)}
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
            packageBatchImport={packageBatchImportWorkflow}
            exports={exportWorkflow}
            volumePackages={volumePackageExportWorkflow}
            volumeFloppies={volumeFloppyExportWorkflow}
            mediaExports={mediaExportWorkflow}
            deletion={deletionWorkflow}
            programGeneration={programGenerationWorkflow}
            mediaDrop={mediaDropWorkflow}
            tx16wImport={tx16wImportWorkflow}
            audioImport={audioImportWorkflow}
            {audioFileInput}
            sequenceImport={sequenceImportWorkflow}
            {sequenceFileInput}
            sampleNames={samples.map((item) => item.name)}
            sampleBankNames={sampleBanks.map((item) => item.name)}
            waveDataNames={waveData.map((item) => item.name)}
            sequenceNames={sequences.map((item) => item.name)}
        />
    {/await}
{/if}
