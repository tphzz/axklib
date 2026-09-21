<script lang="ts">
    import InspectorModeFooter from '../../../lib/components/InspectorModeFooter.svelte';
    import { tick, untrack } from 'svelte';
    import type { LaneQueries } from '../../audition/workflow.svelte';
    import AboutDialog from '../../../lib/components/AboutDialog.svelte';
    import AuditionBar from '../../../lib/components/AuditionBar.svelte';
    import SampleCollection from './SampleCollection.svelte';
    import Icon from '../../../lib/components/Icon.svelte';
    import ImageNavigator from '../../../lib/components/ImageNavigator.svelte';
    import DeviceLowerZone from '../../object-editor/DeviceLowerZone.svelte';
    import ObjectInspector from '../../../lib/components/ObjectInspector.svelte';
    import ObjectWorkspace from '../../../lib/components/ObjectWorkspace.svelte';
    import ProgramWorkspace, { type ProgramPresentation } from '../../../lib/components/ProgramWorkspace.svelte';
    import PackageSelectionControls from '../../../lib/components/PackageSelectionControls.svelte';
    import SequenceWorkspace from '../../sequence/SequenceWorkspace.svelte';
    import type { SystemProgramContexts, SystemProgramPart } from '../../../lib/transport';
    import type {
        DiskTreeItem,
        PackageExportObject,
        Program,
        SampleStructureItem,
        WaveDataItem,
        WorkspaceView,
    } from '../../../lib/types';
    import { revealCollectionObject } from '../../../lib/collectionNavigation';
    import { desktopBuildInfo, type DesktopBuildInfo, type DesktopBuildInfoState } from '../../../lib/desktopBuildInfo';
    import { copyObjectDetailToClipboard } from '../../../lib/objectDetailClipboard';
    import { userFacingMessage } from '../../../lib/userFacingMessage';

    import WorkspaceShell from '../../workspace/WorkspaceShell.svelte';
    import type { WorkspaceMode } from '../../workspace/contracts';
    import ImageActions from '../../../lib/components/ImageActions.svelte';
    import { FilesController } from '../../files/controller.svelte';
    import FilesView from './AxklibFilesView.svelte';
    import FilesNavigation from '../../files/FilesNavigation.svelte';
    import FilesInspector from '../../files/FilesInspector.svelte';
    import FilesExportCoordinator from '../../files/FilesExportCoordinator.svelte';

    import type { WorkspaceProps } from './workspaceProps';

    let {
        mode = $bindable('device'),
        revision = 0,
        transport,
        filesMutations,
        filesExports,
        filesImports,
        interfaceScaling = null,
        isDesktop,
        workspaceTabs,
        workspaceView,
        inspectorOpen = $bindable(true),
        imageLocation,
        sourceItems,
        selectedSource,
        selectedVolumeIds,
        imageOpening,
        sessionId,
        catalog,
        audition,
        mutation,
        audioImport,
        sequenceImport,
        importAudio,
        importMidi,
        programs,
        sampleBanks,
        samples,
        waveData,
        sequences,
        bankMembers,
        bankMemberWaveData,
        sampleWaveData,
        activeCollectionObjectId,
        inspectorSelection,
        editorSelection,
        sourceStatus,
        packageSelection,
        objectDeletionAvailable,
        waveDataCleanupAvailable,
        programGenerationAvailable,
        programAssignmentCleanupAvailable,
        packageImportAvailable,
        packageExportAvailable,
        volumePackageExportAvailable,
        volumeFloppyExportAvailable,
        audioExportAvailable,
        sequenceExportAvailable,
        mediaConversionAvailable,
        allocationInspectionAvailable,
        samplerOrderingEnabled = false,
        openConnectionSettings,
        openImage,
        createImage,
        closeImage,
        showImageIntegrity,
        manageLocations,
        selectSource,
        selectSourceForContext,
        imageAction,
        selectWorkspace,
        exportPackage,
        exportAudio,
        exportWav,
        exportMidi,
        deleteObjects,
        cleanupWaveData,
        generatePrograms,
        cleanupProgramAssignments,
        clearSelection,
        selectionChanged,
        selectionLimit,
        setStatus,
    }: WorkspaceProps = $props();

    let mainStage: HTMLElement;
    let lowerOpen = $state(false);
    let programPresentation = $state<ProgramPresentation>('single');
    let selectedMultiPart = $state<SystemProgramPart | null>(null);
    let observedSessionId = $state<number | null>(null);
    let observedVolumeId = $state('');
    let observedSystemProgramContexts = $state<SystemProgramContexts | null>(null);
    let aboutDialogOpen = $state(false);
    let aboutBuildInfoState = $state<DesktopBuildInfoState>({ status: 'loading' });
    let cachedDesktopBuildInfo: DesktopBuildInfo | null = null;
    let desktopBuildInfoRequest: Promise<DesktopBuildInfo> | null = null;
    const lowerPanelAvailable = $derived(workspaceView !== 'wave-data' && workspaceView !== 'sequences');
    const auditionAvailable = $derived(workspaceView !== 'programs' && workspaceView !== 'sequences');
    const multiPartEditorContext = $derived(
        programPresentation === 'multi' && selectedMultiPart
            ? {
                  partLabel: selectedMultiPart.partLabel,
                  programNumber: selectedMultiPart.programNumber,
              }
            : null,
    );

    $effect(() => {
        if (sessionId === observedSessionId) return;
        observedSessionId = sessionId;
        programPresentation = 'single';
        selectedMultiPart = null;
    });

    $effect(() => {
        if (catalog.activeVolumeId === observedVolumeId) return;
        observedVolumeId = catalog.activeVolumeId;
        selectedMultiPart = null;
    });

    $effect(() => {
        if (catalog.systemProgramContexts === observedSystemProgramContexts) return;
        observedSystemProgramContexts = catalog.systemProgramContexts;
        selectedMultiPart = null;
    });

    function changeProgramPresentation(value: ProgramPresentation): void {
        programPresentation = value;
        clearSelection();
        if (value === 'single') selectedMultiPart = null;
    }

    function selectSingleProgram(program: Program): void {
        selectedMultiPart = null;
        audition.selectProgram(program);
    }

    function selectMultiPart(part: SystemProgramPart, program: Program | null): void {
        selectedMultiPart = part;
        clearSelection();
        if (program) {
            audition.selectProgram(program);
            return;
        }
        catalog.selectedProgramId = '';
        catalog.inspectorObjectId = '';
        catalog.editorObjectIds.programs = '';
    }

    async function navigateInspectorRelationship(objectId: string, focusTarget: boolean): Promise<void> {
        clearSelection();
        const view = await audition.navigateToObject(objectId);
        if (!view) return;
        if (view === 'programs') {
            programPresentation = 'single';
            selectedMultiPart = null;
        }
        await revealCollectionObject(mainStage, view, objectId, 'center', focusTarget);
    }

    async function copyInspectorMetadata(objectId: string): Promise<void> {
        if (sessionId === null) throw new Error('No image is open');
        try {
            const detail = await transport.objectDetail(sessionId, objectId);
            await copyObjectDetailToClipboard(detail);
            setStatus('Copied object metadata to the clipboard');
        } catch (error) {
            setStatus(userFacingMessage(error));
            throw error;
        }
    }

    async function openAbout(): Promise<void> {
        aboutDialogOpen = true;
        if (cachedDesktopBuildInfo) {
            aboutBuildInfoState = { status: 'ready', buildInfo: cachedDesktopBuildInfo };
            return;
        }

        aboutBuildInfoState = { status: 'loading' };
        desktopBuildInfoRequest ??= desktopBuildInfo();
        try {
            cachedDesktopBuildInfo = await desktopBuildInfoRequest;
            aboutBuildInfoState = { status: 'ready', buildInfo: cachedDesktopBuildInfo };
        } catch {
            desktopBuildInfoRequest = null;
            aboutBuildInfoState = { status: 'error' };
        }
    }

    let files = $state<FilesController | null>(null);
    const filesDriver = $derived(sessionId !== null ? filesMutations?.(sessionId) : undefined);
    const exportDriver = $derived(sessionId !== null ? filesExports?.(sessionId) : undefined);
    const importDriver = $derived(sessionId !== null ? filesImports?.(sessionId) : undefined);
    let fileExporter = $state<FilesExportCoordinator>();
    let fileView = $state<{
        importRoot(root: import('../../../lib/filesystem').FilesystemEntry): void;
        importCommands(
            root: import('../../../lib/filesystem').FilesystemEntry,
        ): { label: string; action: () => void }[];
        isBusy(): boolean;
    }>();
    const exportFiles = (entries: import('../../../lib/filesystem').FilesystemEntry[]) =>
        fileExporter?.open(files?.revision ?? 0, entries);
    let previousSession = $state<number | null>(null);
    let revealEntry = $state<import('../../../lib/filesystem').FilesystemEntry | null>(null);
    let mappingGeneration = 0;
    let modeGeneration = 0;
    let deviceScroll: { view: WorkspaceView; scope: string; offsets: number[] } | null = null;
    const deviceScrollers = () => [
        ...mainStage.querySelectorAll<HTMLElement>('[data-navigation-list], .tree-scroll, .inspector-body'),
    ];
    function captureDeviceScroll(): void {
        if (mode === 'device')
            deviceScroll = {
                view: workspaceView,
                scope: selectedSource.id,
                offsets: deviceScrollers().map((node) => node.scrollTop),
            };
    }
    $effect(() => {
        const currentSession = sessionId;
        const currentRevision = revision;
        modeGeneration += 1;
        if (currentSession === null || currentRevision === 0) {
            files = null;
            previousSession = null;
            return;
        }
        const sameSession = untrack(() => previousSession === currentSession);
        if (!sameSession) deviceScroll = null;
        const context = untrack(() => (sameSession ? files?.capture() : undefined));
        previousSession = currentSession;
        const controller = new FilesController({ inspect: (query) => transport.filesystem(currentSession, query) });
        files = controller;
        let active = true;
        void controller.initialize(context).then(() => {
            if (!active || !controller.initialized) return;
            if (!sameSession || !controller.deviceView)
                mode = controller.deviceView === 'a-series' ? 'device' : 'files';
        });
        return () => {
            active = false;
            controller.dispose();
        };
    });
    $effect(() => {
        const controller = files;
        const objectId = catalog.inspectorObjectId;
        const token = ++mappingGeneration;
        revealEntry = null;
        if (!controller?.initialized || !controller.available || !objectId) return;
        void controller
            .lookup({ objectId })
            .then((entry) => {
                if (token === mappingGeneration && files === controller) revealEntry = entry;
            })
            .catch(() => undefined);
    });
    async function changeMode(next: WorkspaceMode): Promise<void> {
        const controller = files;
        const token = ++modeGeneration;
        if (next === mode) return;
        if (next === 'files') {
            if (!controller || (!controller.available && !controller.error)) return;
            if (!controller.initialized) {
                captureDeviceScroll();
                void audition.stop();
                clearSelection();
                mode = next;
                return;
            }
            const entry = await controller.lookup({ contentScopeId: selectedSource.id }).catch(() => null);
            if (controller !== files || token !== modeGeneration) return;
            if (entry) await controller.chooseRoot(entry.rootId);
            if (controller !== files || token !== modeGeneration) return;
            captureDeviceScroll();
            void audition.stop();
            clearSelection();
        } else {
            if (files?.deviceView !== 'a-series') return;
            const scope = files.root?.contentScopeId;
            if (scope) {
                const find = (items: DiskTreeItem[]): DiskTreeItem | undefined => {
                    for (const item of items) {
                        if (item.id === scope) return item;
                        const child = find(item.children ?? []);
                        if (child) return child;
                    }
                    return undefined;
                };
                const item = find(sourceItems);
                if (item && item.id !== selectedSource.id && item.partitionIndex !== selectedSource.partitionIndex)
                    selectSource(item, 'replace', []);
            }
        }
        mode = next;
        if (next === 'device') {
            await tick();
            if (
                controller === files &&
                token === modeGeneration &&
                deviceScroll?.view === workspaceView &&
                deviceScroll.scope === selectedSource.id
            )
                deviceScrollers().forEach((node, index) => (node.scrollTop = deviceScroll?.offsets[index] ?? 0));
        }
    }
    async function showInDevice(objectId: string): Promise<void> {
        if (files?.deviceView !== 'a-series') return;
        mode = 'device';
        await navigateInspectorRelationship(objectId, true);
    }
    async function revealInFiles(): Promise<void> {
        if (!files || !revealEntry) return;
        const controller = files;
        const token = ++modeGeneration;
        if (!(await controller.reveal(revealEntry)) || controller !== files || token !== modeGeneration) return;
        captureDeviceScroll();
        void audition.stop();
        clearSelection();
        mode = 'files';
    }
</script>

{#snippet imageActions()}
    <ImageActions
        image={imageLocation}
        opening={imageOpening}
        storageLocationsAvailable={transport.storageMode === 'server'}
        onopen={openImage}
        oncreate={createImage}
        onclose={closeImage}
        onintegrity={showImageIntegrity}
        onmanagelocations={manageLocations}
    />
{/snippet}
{#snippet deviceNavigation()}
    <ImageNavigator
        navigationOnly
        image={imageLocation}
        items={sourceItems}
        selectedId={selectedSource.id}
        {selectedVolumeIds}
        opening={imageOpening}
        storageLocationsAvailable={transport.storageMode === 'server'}
        onopen={openImage}
        oncreate={createImage}
        onclose={closeImage}
        onintegrity={showImageIntegrity}
        onmanagelocations={manageLocations}
        onselect={selectSource}
        oncontextselect={selectSourceForContext}
        volumeActionsEnabled={mutation.volumeAvailable}
        partitionActionsEnabled={mutation.partitionAvailable}
        packageImportEnabled={packageImportAvailable}
        packageExportEnabled={packageExportAvailable}
        volumePackageExportEnabled={volumePackageExportAvailable}
        volumeFloppyExportEnabled={volumeFloppyExportAvailable}
        audioExportEnabled={audioExportAvailable}
        mediaConversionEnabled={mediaConversionAvailable}
        allocationInspectionEnabled={allocationInspectionAvailable}
        {samplerOrderingEnabled}
        onimageaction={imageAction}
        onloadchildren={(parentId, offset, limit) =>
            sessionId === null
                ? Promise.resolve({ items: [], totalCount: 0 })
                : transport.contentChildren(sessionId, parentId, offset, limit)}
    />
{/snippet}
{#snippet deviceContent()}
    {#if workspaceView === 'sample-banks' || workspaceView === 'samples'}
        <SampleCollection
            {sessionId}
            {revision}
            bind:lowerOpen
            view={workspaceView}
            {sampleBanks}
            samples={workspaceView === 'sample-banks' ? bankMembers : samples}
            waveData={workspaceView === 'sample-banks' ? bankMemberWaveData : sampleWaveData}
            activeSampleBankId={workspaceView === 'sample-banks' ? catalog.selectedBankId : ''}
            activeSampleId={workspaceView === 'sample-banks' ? catalog.selectedBankMemberId : catalog.selectedSampleId}
            activeWaveDataId={workspaceView === 'sample-banks'
                ? catalog.selectedBankWaveDataId
                : catalog.selectedSampleWaveDataId}
            queries={audition.laneQueries[workspaceView]}
            showOnlyStandaloneSamples={audition.showOnlyStandaloneSamples}
            onshowonlystandalonechange={(checked) => audition.updateShowOnlyStandaloneSamples(checked)}
            onquerychange={(lane: keyof LaneQueries, value) => audition.updateLaneQuery(workspaceView, lane, value)}
            onsamplebankselect={(item: SampleStructureItem) => void audition.selectBank(item)}
            onsampleselect={workspaceView === 'sample-banks'
                ? (item: SampleStructureItem) => void audition.selectBankMember(item)
                : (item: SampleStructureItem) => void audition.selectSample(item)}
            onwavedataselect={(item: WaveDataItem) => void audition.selectWaveData(item)}
            onplaysamplebank={(item) => void audition.playSampleBank(item)}
            onplaysample={(item) => void audition.playSample(item)}
            onplaywavedata={(item) => void audition.playContainedWaveData(item)}
            onstop={() => void audition.stop()}
            onimportaudio={importAudio}
            playingSampleBankId={audition.playingSampleBankId}
            playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
            preparingObjectId={audition.state.status === 'preparing' ? audition.state.objectId : null}
            auditionableSampleIds={audition.auditionableSampleObjectIds}
            auditionableSampleBankIds={audition.auditionableSampleBankObjectIds}
            stereoSampleIds={audition.stereoSampleObjectIds}
            objectRenameAvailable={mutation.objectRenameAvailable}
            onrenameobject={(target) => mutation.requestObjectRename(target)}
            sampleBankAssignmentAvailable={mutation.objectRenameAvailable}
            onassignsamplebank={(selectedSamples) => mutation.requestSampleBankAssignment(selectedSamples)}
            {objectDeletionAvailable}
            ondeleteobjects={deleteObjects}
            {packageExportAvailable}
            onexportobjects={exportPackage}
            {audioExportAvailable}
            onexportaudio={exportAudio}
            onexportwav={exportWav}
            selection={packageSelection}
            onselectionchange={selectionChanged}
            onselectionlimit={selectionLimit}
        />
    {:else if workspaceView === 'sequences'}
        <SequenceWorkspace
            {sequences}
            activeObjectId={activeCollectionObjectId}
            query={audition.laneQueries.sequences.primary}
            onquerychange={(value) => audition.updateLaneQuery('sequences', 'primary', value)}
            onselect={(item) => {
                catalog.selectedSequenceId = item.objectId;
                catalog.inspectorObjectId = item.objectId;
                catalog.editorObjectIds.sequences = item.objectId;
            }}
            objectRenameAvailable={mutation.objectRenameAvailable}
            onrenameobject={(target) => mutation.requestObjectRename(target)}
            {objectDeletionAvailable}
            ondeleteobjects={deleteObjects}
            {packageExportAvailable}
            onexportobjects={exportPackage}
            {sequenceExportAvailable}
            onexportmidi={exportMidi}
            sequenceImportAvailable={sequenceImport.dropAvailable()}
            onimportmidi={importMidi}
            selection={packageSelection}
            onselectionchange={selectionChanged}
            onselectionlimit={selectionLimit}
        />
    {:else if workspaceView === 'programs'}
        <ProgramWorkspace
            {programs}
            contexts={catalog.systemProgramContexts}
            contextsLoading={catalog.systemProgramContextsLoading}
            contextsError={catalog.systemProgramContextsError}
            presentation={programPresentation}
            selectedPartNumber={selectedMultiPart?.partNumber ?? null}
            activeObjectId={activeCollectionObjectId}
            query={audition.laneQueries.programs.primary}
            onquerychange={(value) => audition.updateLaneQuery('programs', 'primary', value)}
            onpresentationchange={changeProgramPresentation}
            onprogramselect={selectSingleProgram}
            onpartselect={selectMultiPart}
            objectRenameAvailable={mutation.objectRenameAvailable}
            onrenameobject={(target) => mutation.requestObjectRename(target)}
            {objectDeletionAvailable}
            ondeleteobjects={deleteObjects}
            programGenerationAvailable={programGenerationAvailable && catalog.activeVolumeId !== ''}
            onprogramgeneration={generatePrograms}
            programAssignmentCleanupAvailable={programAssignmentCleanupAvailable && catalog.activeVolumeId !== ''}
            onprogramassignmentcleanup={cleanupProgramAssignments}
            {packageExportAvailable}
            onexportobjects={exportPackage}
            selection={packageSelection}
            onselectionchange={selectionChanged}
            onselectionlimit={selectionLimit}
        />
    {:else}
        <ObjectWorkspace
            {programs}
            {waveData}
            view={workspaceView}
            activeObjectId={activeCollectionObjectId}
            query={audition.laneQueries[workspaceView].primary}
            onquerychange={(value) => audition.updateLaneQuery(workspaceView, 'primary', value)}
            onprogramselect={selectSingleProgram}
            onwavedataselect={(item: WaveDataItem) => void audition.selectWaveData(item)}
            onpreviewrequest={(item) => audition.requestWaveformPreview(item)}
            onplay={(item) => void audition.playWaveData(item)}
            onprefetch={(item) => audition.prefetchObject(item.objectKey)}
            onstop={() => void audition.stop()}
            onseek={(item, ratio) => audition.seekWaveData(item, ratio)}
            playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
            preparingObjectId={audition.state.status === 'preparing' ? audition.state.objectId : null}
            playheadFrame={audition.state.playheadFrame}
            objectRenameAvailable={mutation.objectRenameAvailable}
            onrenameobject={(target) => mutation.requestObjectRename(target)}
            {objectDeletionAvailable}
            ondeleteobjects={deleteObjects}
            waveDataCleanupAvailable={waveDataCleanupAvailable && catalog.activeVolumeId !== ''}
            oncleanupwavedata={cleanupWaveData}
            programGenerationAvailable={programGenerationAvailable && catalog.activeVolumeId !== ''}
            onprogramgeneration={generatePrograms}
            {packageExportAvailable}
            onexportobjects={exportPackage}
            {audioExportAvailable}
            onexportaudio={exportAudio}
            onexportwav={exportWav}
            selection={packageSelection}
            onselectionchange={selectionChanged}
            onselectionlimit={selectionLimit}
        />
    {/if}
{/snippet}
{#snippet devicePlayback()}
    <AuditionBar
        available={auditionAvailable}
        autoplay={audition.autoplay}
        state={audition.state}
        label={audition.label}
        onautoplaychange={(enabled) => (audition.autoplay = enabled)}
    />
{/snippet}
{#snippet deviceLower()}
    <DeviceLowerZone
        {sessionId}
        selection={editorSelection}
        multiPartContext={multiPartEditorContext}
        assignmentQuery={audition.laneQueries.programs.secondary}
        onassignmentquerychange={(value) => (audition.laneQueries.programs.secondary = value)}
        onassignmentselect={(row) => audition.selectAssignment(row)}
    />
{/snippet}
{#snippet deviceInspector()}
    <div class="device-inspector-zone">
        <ObjectInspector
            selection={inspectorSelection}
            playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
            playheadFrame={audition.state.playheadFrame}
            onrelationshipnavigate={(objectId, focusTarget) =>
                void navigateInspectorRelationship(objectId, focusTarget)}
            onmetadatacopy={copyInspectorMetadata}
        />

        {#if revealEntry}<InspectorModeFooter mode="files" onclick={() => void revealInFiles()} />{/if}
    </div>
{/snippet}
{#snippet deviceTabs()}
    <nav class="workspace-tabs" aria-label="Workspace views">
        {#each workspaceTabs as tab (tab.id)}
            <button class:active={workspaceView === tab.id} type="button" onclick={() => selectWorkspace(tab.id)}>
                <Icon name={tab.icon} size={16} /><span>{tab.label}</span>
            </button>
        {/each}
    </nav>
{/snippet}
{#snippet deviceActions()}
    {#if (packageExportAvailable || audioExportAvailable || sequenceExportAvailable || objectDeletionAvailable) && packageSelection.items.length > 0}
        <PackageSelectionControls
            count={packageSelection.items.length}
            onexportpackage={packageExportAvailable ? () => exportPackage(packageSelection.items) : undefined}
            onexportsfz={audioExportAvailable ? () => exportAudio(packageSelection.items) : undefined}
            onexportmidi={sequenceExportAvailable && packageSelection.items.every((item) => item.kind === 'SEQU')
                ? () => exportMidi(packageSelection.items as PackageExportObject[])
                : undefined}
            ondelete={objectDeletionAvailable ? () => deleteObjects(packageSelection.items) : undefined}
            onclear={clearSelection}
        />
    {/if}
{/snippet}
{#snippet fileNavigation()}
    {#if files}<FilesNavigation
            controller={files}
            onexport={exportDriver ? exportFiles : undefined}
            onimport={importDriver ? (root) => fileView?.importRoot(root) : undefined}
            additionalImports={(root) => fileView?.importCommands(root) ?? []}
            exportBlocked={fileExporter?.isOpen() || fileView?.isBusy()}
        />{/if}
{/snippet}
{#snippet fileContent()}
    {#if files}{#key sessionId}<FilesView
                bind:this={fileView}
                controller={files}
                sessionId={sessionId!}
                driver={filesDriver}
                imports={importDriver}
                exports={exportDriver}
                onexport={exportDriver ? exportFiles : undefined}
                exportBlocked={fileExporter?.isOpen()}
                {setStatus}
            />{/key}{:else}<p class="empty-copy">No image open</p>{/if}
{/snippet}
{#snippet fileInspector()}
    {#if files}<FilesInspector
            entry={files.selected ?? files.root}
            canShowDevice={files.deviceView === 'a-series'}
            interpretationLabel={(
                { PROG: 'Program', SBAC: 'Sample Bank', SBNK: 'Sample', SMPL: 'Wave Data', SEQU: 'Sequence' } as Record<
                    string,
                    string
                >
            )[files.selected?.interpretation ?? '']}
            onshowdevice={(id) => void showInDevice(id)}
        />{/if}
{/snippet}

<div bind:this={mainStage} class="backend-workspace">
    <WorkspaceShell
        {mode}
        {imageActions}
        {interfaceScaling}
        {isDesktop}
        bind:inspectorOpen
        bind:lowerOpen
        imageName={imageLocation?.displayName.split(/[\\/]/).at(-1) ?? ''}
        context={files?.filesystemName ?? ''}
        status={mode === 'device'
            ? sourceStatus
            : files?.error ||
              (files?.busy
                  ? 'Reading filesystem'
                  : filesDriver && files?.capabilities?.createDirectory
                    ? 'Files · Writable'
                    : 'Files · Read only')}
        count={mode === 'device' ? `${catalog.objectCount} objects` : `${files?.rows.length ?? 0} entries`}
        deviceAvailable={sessionId === null || !files?.initialized || files.deviceView === 'a-series'}
        filesAvailable={Boolean(files?.initialized && files.available) || Boolean(files?.error)}
        onmodechange={(value) => void changeMode(value)}
        onabout={openAbout}
        onconnection={openConnectionSettings}
        presentation={mode === 'device'
            ? {
                  navigation: deviceNavigation,
                  content: deviceContent,
                  inspector: deviceInspector,
                  lower: lowerPanelAvailable ? deviceLower : undefined,
                  lowerPreferredHeight:
                      editorSelection?.kind === 'sample' || editorSelection?.kind === 'sample-bank' ? 360 : undefined,
                  tabs: deviceTabs,
                  playback: auditionAvailable ? devicePlayback : undefined,
                  selectionActions: deviceActions,
              }
            : { navigation: fileNavigation, content: fileContent, inspector: fileInspector }}
    />
</div>
{#if aboutDialogOpen}<AboutDialog state={aboutBuildInfoState} onclose={() => (aboutDialogOpen = false)} />{/if}
{#if exportDriver}{#key sessionId}<FilesExportCoordinator
            driver={exportDriver}
            {setStatus}
            bind:this={fileExporter}
        />{/key}{/if}

<style>
    .backend-workspace {
        display: contents;
    }
    .device-inspector-zone {
        display: flex;
        flex-direction: column;
        min-height: 0;
        min-width: 0;
        overflow: hidden;
        border-left: 1px solid var(--color-border);
        background: var(--color-panel);
    }
    .device-inspector-zone :global(.inspector) {
        flex: 1;
        min-height: 0;
        border: 0;
    }
</style>
