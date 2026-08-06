<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import type { AuditionWorkflow, LaneQueries } from '../audition/workflow.svelte';
    import type { CatalogWorkflow } from '../catalog/workflow.svelte';
    import type { AudioImportWorkflow } from '../import/audioWorkflow.svelte';
    import type { SequenceImportWorkflow } from '../import/sequenceWorkflow.svelte';
    import type { MutationWorkflow } from '../mutation/workflow.svelte';
    import AuditionBar from '../../lib/components/AuditionBar.svelte';
    import ContainedObjectWorkspace from '../../lib/components/ContainedObjectWorkspace.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import ImageNavigator from '../../lib/components/ImageNavigator.svelte';
    import LayoutControls from '../../lib/components/LayoutControls.svelte';
    import ObjectEditor from '../../lib/components/ObjectEditor.svelte';
    import ObjectInspector from '../../lib/components/ObjectInspector.svelte';
    import ObjectWorkspace from '../../lib/components/ObjectWorkspace.svelte';
    import PackageSelectionControls from '../../lib/components/PackageSelectionControls.svelte';
    import SequenceWorkspace from '../sequence/SequenceWorkspace.svelte';
    import type { InterfaceScaleController, InterfaceScaleMode, InterfaceScaleState } from '../../lib/interfaceScale';
    import type { ImageLocation } from '../../lib/storageLocations';
    import type { ImageTransport } from '../../lib/transport';
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
    import type { PackageExportSelectionState } from '../../lib/objectSelection';

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
        imageOpening: boolean;
        sessionId: number | null;
        catalog: CatalogWorkflow;
        audition: AuditionWorkflow;
        mutation: MutationWorkflow;
        audioImport: AudioImportWorkflow;
        sequenceImport: SequenceImportWorkflow;
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
        packageImportAvailable: boolean;
        packageExportAvailable: boolean;
        volumePackageExportAvailable: boolean;
        volumeFloppyExportAvailable: boolean;
        audioExportAvailable: boolean;
        sequenceExportAvailable: boolean;
        mediaConversionAvailable: boolean;
        openConnectionSettings: () => void;
        openImage: () => void;
        createImage: () => void;
        closeImage: () => void;
        manageLocations: () => void;
        selectSource: (item: DiskTreeItem) => void;
        imageAction: (item: DiskTreeItem, action: ImageTreeAction) => void;
        selectWorkspace: (view: WorkspaceView) => void;
        exportPackage: (items: PackageExportObject[]) => void;
        exportAudio: (items: PackageExportSelection[]) => void;
        exportMidi: (items: PackageExportObject[]) => void;
        deleteObjects: (items: PackageExportObject[]) => void;
        cleanupWaveData: () => void;
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
        imageOpening,
        sessionId,
        catalog,
        audition,
        mutation,
        audioImport,
        sequenceImport,
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
        packageImportAvailable,
        packageExportAvailable,
        volumePackageExportAvailable,
        volumeFloppyExportAvailable,
        audioExportAvailable,
        sequenceExportAvailable,
        mediaConversionAvailable,
        openConnectionSettings,
        openImage,
        createImage,
        closeImage,
        manageLocations,
        selectSource,
        imageAction,
        selectWorkspace,
        exportPackage,
        exportAudio,
        exportMidi,
        deleteObjects,
        cleanupWaveData,
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
    const lowerPanelAvailable = $derived(workspaceView !== 'wave-data' && workspaceView !== 'sequences');
    const auditionAvailable = $derived(workspaceView !== 'programs' && workspaceView !== 'sequences');

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
</script>

<div class:sidebar-closed={!sidebarOpen} class:inspector-closed={!inspectorOpen} class="app-shell">
    <header class="app-header">
        <div class="brand">
            <span class="brand-mark"><Icon name="waveform" size={20} strokeWidth={2.1} /></span><strong>axkdeck</strong>
        </div>
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
            opening={imageOpening}
            storageLocationsAvailable={transport.storageMode === 'server'}
            onopen={openImage}
            oncreate={createImage}
            onclose={closeImage}
            onmanagelocations={manageLocations}
            onselect={selectSource}
            volumeActionsEnabled={mutation.volumeAvailable}
            partitionActionsEnabled={mutation.partitionAvailable}
            packageImportEnabled={packageImportAvailable}
            packageExportEnabled={packageExportAvailable}
            volumePackageExportEnabled={volumePackageExportAvailable}
            volumeFloppyExportEnabled={volumeFloppyExportAvailable}
            audioExportEnabled={audioExportAvailable}
            mediaConversionEnabled={mediaConversionAvailable}
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
        data-import-drop-main={audioImport.activeTarget() || sequenceImport.activeTarget() ? 'true' : undefined}
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
                onimportaudio={() => audioImport.chooseFiles()}
                playingSampleBankId={audition.playingSampleBankId}
                playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
                preparingObjectId={audition.state.status === 'preparing' ? audition.state.objectId : null}
                auditionableSampleIds={audition.auditionableSampleObjectIds}
                auditionableSampleBankIds={audition.auditionableSampleBankObjectIds}
                objectRenameAvailable={mutation.objectRenameAvailable}
                onrenameobject={(target) => mutation.requestObjectRename(target)}
                sampleBankCreationAvailable={mutation.objectRenameAvailable}
                oncreatesamplebank={(selectedSamples) => mutation.requestSampleBankCreation(selectedSamples)}
                {objectDeletionAvailable}
                ondeleteobjects={deleteObjects}
                {packageExportAvailable}
                onexportobjects={exportPackage}
                {audioExportAvailable}
                onexportaudio={exportAudio}
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
                sequenceImportAvailable={mutation.objectRenameAvailable && sequenceImport.activeTarget() !== null}
                onimportmidi={() => sequenceImport.chooseFiles()}
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
                onprogramselect={(program) => audition.selectProgram(program)}
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
                {packageExportAvailable}
                onexportobjects={exportPackage}
                {audioExportAvailable}
                onexportaudio={exportAudio}
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
