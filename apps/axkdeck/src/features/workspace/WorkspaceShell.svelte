<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import type { AuditionWorkflow, LaneQueries } from '../audition/workflow.svelte';
    import type { CatalogWorkflow } from '../catalog/workflow.svelte';
    import type { AudioImportWorkflow } from '../import/audioWorkflow.svelte';
    import type { SequenceImportWorkflow } from '../import/sequenceWorkflow.svelte';
    import type { MutationWorkflow } from '../mutation/workflow.svelte';
    import AboutDialog from '../../lib/components/AboutDialog.svelte';
    import AuditionBar from '../../lib/components/AuditionBar.svelte';
    import ContainedObjectWorkspace from '../../lib/components/ContainedObjectWorkspace.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import ImageNavigator from '../../lib/components/ImageNavigator.svelte';
    import LayoutControls from '../../lib/components/LayoutControls.svelte';
    import ObjectEditor from '../../lib/components/ObjectEditor.svelte';
    import ObjectInspector from '../../lib/components/ObjectInspector.svelte';
    import ObjectWorkspace from '../../lib/components/ObjectWorkspace.svelte';
    import ProgramWorkspace, { type ProgramPresentation } from '../../lib/components/ProgramWorkspace.svelte';
    import PackageSelectionControls from '../../lib/components/PackageSelectionControls.svelte';
    import SequenceWorkspace from '../sequence/SequenceWorkspace.svelte';
    import type { InterfaceScaleController, InterfaceScaleMode, InterfaceScaleState } from '../../lib/interfaceScale';
    import type { ImageLocation } from '../../lib/storageLocations';
    import type { ImageTransport, SystemProgramContexts, SystemProgramPart } from '../../lib/transport';
    import type {
        DiskTreeItem,
        ImageTreeAction,
        InspectorSelection,
        PackageExportObject,
        PackageExportSelection,
        Program,
        SampleStructureItem,
        SequenceItem,
        WaveDataItem,
        WorkspaceView,
    } from '../../lib/types';
    import type { ObjectSelectionMode, PackageExportSelectionState } from '../../lib/objectSelection';
    import { desktopBuildInfo, type DesktopBuildInfo, type DesktopBuildInfoState } from '../../lib/desktopBuildInfo';

    interface WorkspaceTab {
        id: WorkspaceView;
        label: string;
        icon: 'music' | 'layers' | 'archive' | 'waveform' | 'list';
    }

    interface Props {
        transport: ImageTransport;
        interfaceScaling?: InterfaceScaleController | null;
        isDesktop: boolean;
        workspaceTabs: WorkspaceTab[];
        workspaceView: WorkspaceView;
        inspectorOpen?: boolean;
        imageLocation: ImageLocation | null;
        sourceItems: DiskTreeItem[];
        selectedSource: DiskTreeItem;
        selectedVolumeIds: readonly string[];
        imageOpening: boolean;
        sessionId: number | null;
        catalog: CatalogWorkflow;
        audition: AuditionWorkflow;
        mutation: MutationWorkflow;
        audioImport: AudioImportWorkflow;
        sequenceImport: SequenceImportWorkflow;
        importAudio: () => void;
        importMidi: () => void;
        programs: Program[];
        sampleBanks: SampleStructureItem[];
        samples: SampleStructureItem[];
        waveData: WaveDataItem[];
        sequences: SequenceItem[];
        bankMembers: SampleStructureItem[];
        bankMemberWaveData: WaveDataItem[];
        sampleWaveData: WaveDataItem[];
        activeCollectionObjectId: string;
        inspectorSelection: InspectorSelection;
        editorSelection: InspectorSelection;
        sourceStatus: string;
        packageSelection: PackageExportSelectionState;
        objectDeletionAvailable: boolean;
        waveDataCleanupAvailable: boolean;
        programGenerationAvailable: boolean;
        packageImportAvailable: boolean;
        packageExportAvailable: boolean;
        volumePackageExportAvailable: boolean;
        volumeFloppyExportAvailable: boolean;
        audioExportAvailable: boolean;
        sequenceExportAvailable: boolean;
        mediaConversionAvailable: boolean;
        allocationInspectionAvailable: boolean;
        samplerOrderingEnabled?: boolean;
        openConnectionSettings: () => void;
        openImage: () => void;
        createImage: () => void;
        closeImage: () => void;
        showImageIntegrity: () => void;
        manageLocations: () => void;
        selectSource: (item: DiskTreeItem, mode: ObjectSelectionMode, visibleVolumes: readonly DiskTreeItem[]) => void;
        selectSourceForContext: (item: DiskTreeItem, visibleVolumes: readonly DiskTreeItem[]) => void;
        imageAction: (item: DiskTreeItem, action: ImageTreeAction) => void;
        selectWorkspace: (view: WorkspaceView) => void;
        exportPackage: (items: PackageExportObject[]) => void;
        exportAudio: (items: PackageExportSelection[]) => void;
        exportWav: (items: PackageExportSelection[]) => void;
        exportMidi: (items: PackageExportObject[]) => void;
        deleteObjects: (items: PackageExportObject[]) => void;
        cleanupWaveData: () => void;
        generatePrograms: () => void;
        clearSelection: () => void;
        selectionChanged: (selection: PackageExportSelectionState) => void;
        selectionLimit: () => void;
    }

    let {
        transport,
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
        clearSelection,
        selectionChanged,
        selectionLimit,
    }: Props = $props();

    let sidebarOpen = $state(true);
    let lowerPanelOpen = $state(false);
    let splitRatio = $state(2 / 3);
    let mainStage: HTMLElement;
    let resizing = $state(false);
    let interfaceScaleState = $state<InterfaceScaleState | null>(null);
    let stopInterfaceScaleSubscription: (() => void) | undefined;
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

    onMount(() => {
        interfaceScaleState = interfaceScaling?.state() ?? null;
        stopInterfaceScaleSubscription = interfaceScaling?.subscribe((state) => {
            interfaceScaleState = state;
        });
    });

    onDestroy(() => {
        stopInterfaceScaleSubscription?.();
        void interfaceScaling?.dispose();
    });

    function setInterfaceScale(mode: InterfaceScaleMode): void {
        void interfaceScaling?.setMode(mode);
    }

    function resizeSplit(clientY: number): void {
        const bounds = mainStage.getBoundingClientRect();
        const available = Math.max(1, bounds.height - (auditionAvailable ? 34 : 4));
        const minimum = Math.min(180, available / 2);
        const top = Math.min(available - minimum, Math.max(minimum, clientY - bounds.top));
        splitRatio = top / available;
    }

    function startResize(event: PointerEvent): void {
        resizing = true;
        event.currentTarget instanceof HTMLElement && event.currentTarget.setPointerCapture(event.pointerId);
        resizeSplit(event.clientY);
    }

    function moveResize(event: PointerEvent): void {
        if (resizing) resizeSplit(event.clientY);
    }

    function stopResize(): void {
        resizing = false;
    }

    function resizeWithKeyboard(event: KeyboardEvent): void {
        if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
        event.preventDefault();
        const available = Math.max(1, mainStage.getBoundingClientRect().height - (auditionAvailable ? 34 : 4));
        const delta = (event.shiftKey ? 64 : 16) / available;
        splitRatio = Math.min(0.8, Math.max(0.2, splitRatio + (event.key === 'ArrowDown' ? delta : -delta)));
    }

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
</script>

<div class:sidebar-closed={!sidebarOpen} class:inspector-closed={!inspectorOpen} class="app-shell">
    <header class="app-header">
        <button class="brand" type="button" aria-label="About axkdeck" title="About axkdeck" onclick={openAbout}>
            <span class="brand-mark"><Icon name="waveform" size={20} strokeWidth={2.1} /></span><strong>axkdeck</strong>
        </button>
        <nav class="workspace-tabs" aria-label="Workspace views">
            {#each workspaceTabs as tab (tab.id)}
                <button class:active={workspaceView === tab.id} type="button" onclick={() => selectWorkspace(tab.id)}>
                    <Icon name={tab.icon} size={16} /><span>{tab.label}</span>
                </button>
            {/each}
        </nav>
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
        <div class="global-controls">
            {#if isDesktop}
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Server connection settings"
                    title="Server connection settings"
                    onclick={openConnectionSettings}><Icon name="server" size={17} /></button
                >
            {/if}
            <LayoutControls
                libraryOpen={sidebarOpen}
                editorOpen={lowerPanelOpen && lowerPanelAvailable}
                editorAvailable={lowerPanelAvailable}
                {inspectorOpen}
                interfaceScale={interfaceScaleState}
                ontogglelibrary={() => (sidebarOpen = !sidebarOpen)}
                ontoggleeditor={() => (lowerPanelOpen = !lowerPanelOpen)}
                ontoggleinspector={() => (inspectorOpen = !inspectorOpen)}
                oninterfacescalechange={setInterfaceScale}
            />
        </div>
    </header>

    {#if sidebarOpen}
        <ImageNavigator
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
    {/if}

    <main
        bind:this={mainStage}
        class:lower-panel-closed={!lowerPanelOpen || !lowerPanelAvailable}
        class:has-audition-bar={auditionAvailable}
        class="main-stage"
        style:--split-position={`${splitRatio * 100}%`}
        data-import-drop-main={audioImport.dropAvailable() || sequenceImport.dropAvailable() ? 'true' : undefined}
    >
        {#if workspaceView === 'sample-banks' || workspaceView === 'samples'}
            <ContainedObjectWorkspace
                view={workspaceView}
                {sampleBanks}
                samples={workspaceView === 'sample-banks' ? bankMembers : samples}
                waveData={workspaceView === 'sample-banks' ? bankMemberWaveData : sampleWaveData}
                activeSampleBankId={workspaceView === 'sample-banks' ? catalog.selectedBankId : ''}
                activeSampleId={workspaceView === 'sample-banks'
                    ? catalog.selectedBankMemberId
                    : catalog.selectedSampleId}
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
                sampleBankCreationAvailable={mutation.objectRenameAvailable}
                oncreatesamplebank={(selectedSamples) => mutation.requestSampleBankCreation(selectedSamples)}
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
        <AuditionBar
            available={auditionAvailable}
            autoplay={audition.autoplay}
            state={audition.state}
            label={audition.label}
            onautoplaychange={(enabled) => (audition.autoplay = enabled)}
        />
        {#if lowerPanelOpen && lowerPanelAvailable}
            <!-- Svelte does not model the ARIA separator interaction pattern; pointer and keyboard handlers are intentional. -->
            <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
            <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
            <div
                class:resizing
                class="horizontal-splitter"
                role="separator"
                aria-label="Resize editor panel"
                aria-orientation="horizontal"
                tabindex="0"
                onpointerdown={startResize}
                onpointermove={moveResize}
                onpointerup={stopResize}
                onpointercancel={stopResize}
                onkeydown={resizeWithKeyboard}
            >
                <span></span>
            </div>
            <ObjectEditor
                selection={editorSelection}
                multiPartContext={multiPartEditorContext}
                assignmentQuery={audition.laneQueries.programs.secondary}
                onassignmentquerychange={(value) => (audition.laneQueries.programs.secondary = value)}
                onassignmentselect={(row) => audition.selectAssignment(row)}
            />
        {/if}
    </main>

    {#if inspectorOpen}
        <ObjectInspector
            selection={inspectorSelection}
            playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
            playheadFrame={audition.state.playheadFrame}
        />
    {/if}
    <footer class="status-bar">
        <span><span class="status-dot"></span> {sourceStatus}</span><span class="ml-auto"
            >{catalog.objectCount} objects</span
        >
    </footer>
</div>

{#if aboutDialogOpen}
    <AboutDialog state={aboutBuildInfoState} onclose={() => (aboutDialogOpen = false)} />
{/if}
