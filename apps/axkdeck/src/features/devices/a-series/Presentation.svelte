<script lang="ts">
    import InspectorModeFooter from '../../../lib/components/InspectorModeFooter.svelte';
    import { onDestroy, untrack, type Snippet } from 'svelte';
    import type { RememberView } from '../../workspace/viewMemory';
    import type { LaneQueries } from '../../audition/workflow.svelte';
    import AuditionBar from '../../../lib/components/AuditionBar.svelte';
    import SampleCollection from '../../backends/axklib/SampleCollection.svelte';
    import Icon from '../../../lib/components/Icon.svelte';
    import ImageNavigator from '../../../lib/components/ImageNavigator.svelte';
    import DeviceLowerZone from '../../object-editor/DeviceLowerZone.svelte';
    import ObjectInspector from '../../../lib/components/ObjectInspector.svelte';
    import VolumeInspector from '../../../lib/components/VolumeInspector.svelte';
    import { provideVolumeCapacity } from '../../../lib/volumeCapacity.svelte';
    import { capacitySelection } from '../../../lib/capacitySelection';
    import ObjectWorkspace from '../../../lib/components/ObjectWorkspace.svelte';
    import type { ProgramPresentation } from '../../../lib/components/ProgramWorkspace.svelte';
    import ProgramCollection from '../../backends/axklib/ProgramCollection.svelte';
    import PackageSelectionControls from '../../../lib/components/PackageSelectionControls.svelte';
    import SequenceWorkspace from '../../sequence/SequenceWorkspace.svelte';
    import type { SystemProgramContexts, SystemProgramPart } from '../../../lib/transport';
    import type { DiskTreeItem, PackageExportObject, Program } from '../../../lib/types';
    import { revealCollectionObject } from '../../../lib/collectionNavigation';
    import { copySessionObjectMetadata } from '../../../lib/objectDetailClipboard';

    import type { WorkspacePresentation, DevicePresentationState } from '../../workspace/contracts';

    import type { WorkspaceProps } from '../../backends/axklib/workspaceProps';

    let {
        workspace,
        children,
        canRevealFiles,
        onrevealFiles,
        remember,
    }: {
        workspace: WorkspaceProps;
        children: Snippet<[WorkspacePresentation, DevicePresentationState]>;
        canRevealFiles: boolean;
        onrevealFiles: () => void;
        remember: RememberView;
    } = $props();
    let {
        revision = 0,
        transport,
        workspaceTabs,
        workspaceView,
        imageLocation,
        sourceItems,
        selectedSource,
        selectedVolumeIds,
        imageOpening,
        sessionId,
        catalog,
        audition,
        mutation,
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
        samplerOrderingEnabled: enabled = false,
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
    } = $derived(workspace);

    let mainStage: HTMLElement;
    const capacity = provideVolumeCapacity(() => ({ transport, sessionId, revision, enabled, selectedSource }));
    const capacitySelect = capacitySelection(capacity, () => audition);
    const retained = untrack(() =>
        remember('a-series', () => ({
            lowerOpen: false,
            programPresentation: 'single' as ProgramPresentation,
            partNumber: null as number | null,
            volumeId: '',
        })),
    );
    let lowerOpen = $state(retained.lowerOpen);
    let programPresentation = $state<ProgramPresentation>(retained.programPresentation);
    let selectedMultiPart = $state<SystemProgramPart | null>(
        untrack(() => {
            const file = catalog.systemProgramContexts?.files.find((item) => item.fileKind === 'SYSTEM2');
            return retained.volumeId === catalog.activeVolumeId &&
                file?.fileKind === 'SYSTEM2' &&
                file.availability === 'AVAILABLE'
                ? (file.parts.find((part) => part.partNumber === retained.partNumber) ?? null)
                : null;
        }),
    );
    let observedSessionId = $state<number | null>(untrack(() => sessionId));
    let observedVolumeId = $state(untrack(() => catalog.activeVolumeId));
    let observedSystemProgramContexts = $state<SystemProgramContexts | null>(
        untrack(() => catalog.systemProgramContexts),
    );
    onDestroy(() => {
        retained.lowerOpen = lowerOpen;
        retained.programPresentation = programPresentation;
        retained.partNumber = selectedMultiPart?.partNumber ?? null;
        retained.volumeId = observedVolumeId;
    });
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
        capacity.showObject();
        lowerOpen = true;
        selectedMultiPart = null;
        audition.selectProgram(program);
    }

    function selectMultiPart(part: SystemProgramPart, program: Program | null): void {
        capacity.showObject();
        selectedMultiPart = part;
        clearSelection();
        if (program) {
            lowerOpen = true;
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
            lowerOpen = true;
            programPresentation = 'single';
            selectedMultiPart = null;
        }
        await revealCollectionObject(mainStage, view, objectId, 'center', focusTarget);
    }

    export async function showObject(id: string): Promise<void> {
        await navigateInspectorRelationship(id, true);
    }

    export function contentScopeId(): string {
        return selectedSource.id;
    }

    export function showScope(id: string): void {
        const find = (items: DiskTreeItem[]): DiskTreeItem | undefined => {
            for (const item of items) {
                if (item.id === id) return item;
                const nested = find(item.children ?? []);
                if (nested) return nested;
            }
            return undefined;
        };
        const item = find(sourceItems);
        if (item && item.id !== selectedSource.id) selectSource(item, 'replace', []);
    }

    async function copyInspectorMetadata(objectId: string): Promise<void> {
        await copySessionObjectMetadata(transport, sessionId, objectId, setStatus);
    }
</script>

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
        samplerOrderingEnabled={enabled}
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
            onsamplebankselect={capacitySelect.bank}
            onsampleselect={workspaceView === 'sample-banks' ? capacitySelect.member : capacitySelect.sample}
            onwavedataselect={capacitySelect.wave}
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
                capacity.showObject();
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
        <ProgramCollection
            {programs}
            {sessionId}
            {catalog}
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
            onwavedataselect={capacitySelect.wave}
            onpreviewrequest={(item, bins) => audition.requestWaveformPreview(item, bins)}
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
        {#if capacity.selectedVolume && enabled}
            <VolumeInspector item={capacity.selectedVolume} />
        {:else}<ObjectInspector
                selection={inspectorSelection}
                playingObjectId={audition.state.status === 'playing' ? audition.state.objectId : null}
                playheadFrame={audition.state.playheadFrame}
                onrelationshipnavigate={(objectId, focusTarget) =>
                    void navigateInspectorRelationship(objectId, focusTarget)}
                onmetadatacopy={copyInspectorMetadata}
            />{/if}

        {#if canRevealFiles}<InspectorModeFooter mode="files" onclick={onrevealFiles} />{/if}
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

<div bind:this={mainStage} class="backend-workspace">
    {@render children(
        {
            navigation: deviceNavigation,
            content: deviceContent,
            inspector: deviceInspector,
            lower: lowerPanelAvailable ? deviceLower : undefined,
            lowerPreferredHeight:
                editorSelection?.kind === 'sample' || editorSelection?.kind === 'sample-bank' ? 360 : undefined,
            tabs: deviceTabs,
            playback: auditionAvailable ? devicePlayback : undefined,
            selectionActions: deviceActions,
        },
        {
            lowerOpen,
            setLowerOpen: (value) => (lowerOpen = value),
            showObject: (id) => navigateInspectorRelationship(id, true),
        },
    )}
</div>

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
