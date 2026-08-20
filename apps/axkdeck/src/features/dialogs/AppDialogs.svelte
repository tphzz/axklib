<script lang="ts">
    import { onDestroy } from 'svelte';
    import type { DeletionWorkflow } from '../deletion/workflow.svelte';
    import type { PickerRequest, PickerSelection } from './picker';
    import type { ExportWorkflow } from '../export/workflow.svelte';
    import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
    import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
    import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
    import type { AudioImportWorkflow } from '../import/audioWorkflow.svelte';
    import type { MediaDropWorkflow } from '../import/mediaDropWorkflow.svelte';
    import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
    import type { PackageBatchImportWorkflow } from '../import/packageBatchWorkflow.svelte';
    import type { SequenceImportWorkflow } from '../import/sequenceWorkflow.svelte';
    import type { Tx16wImportWorkflow } from '../import/tx16wWorkflow.svelte';
    import type { MutationWorkflow } from '../mutation/workflow.svelte';
    import type { ProgramGenerationWorkflow } from '../program-generation/workflow.svelte';
    import AudioImportDialog from '../../lib/components/AudioImportDialog.svelte';
    import CompanionDiskDialog from '../../lib/components/CompanionDiskDialog.svelte';
    import CreateHardDiskImageDialog from '../../lib/components/CreateHardDiskImageDialog.svelte';
    import CreateSampleBankDialog from '../../lib/components/CreateSampleBankDialog.svelte';
    import AssignSampleBankDialog from '../../lib/components/AssignSampleBankDialog.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import ImportUnavailableDialog from '../../lib/components/ImportUnavailableDialog.svelte';
    import ObjectDeletionDialog from '../../lib/components/ObjectDeletionDialog.svelte';
    import ObjectRenameDialog from '../../lib/components/ObjectRenameDialog.svelte';
    import PackageExportDialog from '../../lib/components/PackageExportDialog.svelte';
    import ExportProgressDialog from '../../lib/components/ExportProgressDialog.svelte';
    import PackageImportDialog from '../../lib/components/PackageImportDialog.svelte';
    import PackageBatchImportDialog from '../../lib/components/PackageBatchImportDialog.svelte';
    import PlacementRepairDialog from '../../lib/components/PlacementRepairDialog.svelte';
    import ProgramGenerationDialog from '../../lib/components/ProgramGenerationDialog.svelte';
    import MidiExportDialog from '../../lib/components/MidiExportDialog.svelte';
    import MidiImportDialog from '../../lib/components/MidiImportDialog.svelte';
    import Tx16wImportDialog from '../../lib/components/Tx16wImportDialog.svelte';
    import MediaExportDialog from '../../lib/components/MediaExportDialog.svelte';
    import ServerConnectionSettings from '../../lib/components/ServerConnectionSettings.svelte';
    import ServerStoragePicker from '../../lib/components/ServerStoragePicker.svelte';
    import SfzExportDialog from '../../lib/components/SfzExportDialog.svelte';
    import VolumeActionDialog from '../../lib/components/VolumeActionDialog.svelte';
    import VolumePackageExportDialog from '../../lib/components/VolumePackageExportDialog.svelte';
    import VolumeFloppyExportDialog from '../../lib/components/VolumeFloppyExportDialog.svelte';
    import WaveDataCleanupDialog from '../../lib/components/WaveDataCleanupDialog.svelte';
    import WorkspaceManager from '../../lib/components/WorkspaceManager.svelte';
    import type { RemoteServerSettingsInput, RemoteServerSettingsView } from '../../lib/serverSettings';
    import type { DirectoryLocation, FileLocation, ImageLocation } from '../../lib/storageLocations';
    import type { CompanionSelection, ImageTransport } from '../../lib/transport';
    import {
        directComputerDialogVisible,
        type DirectComputerOperation,
    } from '../file-operations/directComputerWorkflow';
    import { DelayedExportProgressVisibility, type ExportProgressOperation } from '../export/progressVisibility.svelte';

    interface CompanionDialogState {
        sources: ImageLocation[];
        sourceKind: 'file' | 'directory';
        setLabel: string;
        nextRequiredIndex: number | null;
        busy: boolean;
        error: string;
    }

    interface Props {
        transport: ImageTransport;
        isDesktop: boolean;
        directComputerOperation: DirectComputerOperation | null;
        pickerRequest: PickerRequest | null;
        finishPicker: (selection: PickerSelection | null) => void;
        manageLocations: () => void;
        companionRequest: CompanionDialogState | null;
        addCompanion: () => void;
        removeCompanion: (source: ImageLocation) => void;
        attachCompanions: (selection: CompanionSelection) => void;
        cancelCompanions: () => void;
        hardDiskDirectory: DirectoryLocation | null;
        finishHardDisk: (file: FileLocation) => void;
        cancelHardDisk: () => void;
        workspaceManagerOpen: boolean;
        activeWorkspaceId: string | null;
        closeWorkspaceManager: () => void;
        connectionSettings: RemoteServerSettingsView | null;
        saveRemoteConnection: (input: RemoteServerSettingsInput) => Promise<void>;
        switchToLocalConnection: () => Promise<void>;
        closeConnectionSettings: () => void;
        mutation: MutationWorkflow;
        packageImport: PackageImportWorkflow;
        packageBatchImport: PackageBatchImportWorkflow;
        exports: ExportWorkflow;
        volumePackages: VolumePackageExportWorkflow;
        volumeFloppies: VolumeFloppyExportWorkflow;
        mediaExports: MediaExportWorkflow;
        deletion: DeletionWorkflow;
        programGeneration: ProgramGenerationWorkflow;
        mediaDrop: MediaDropWorkflow;
        audioImport: AudioImportWorkflow;
        audioFileInput?: HTMLInputElement;
        sequenceImport: SequenceImportWorkflow;
        tx16wImport: Tx16wImportWorkflow;
        sequenceFileInput?: HTMLInputElement;
        sampleNames: string[];
        sampleBankNames: string[];
        waveDataNames: string[];
        sequenceNames: string[];
    }

    let {
        transport,
        isDesktop,
        directComputerOperation,
        pickerRequest,
        finishPicker,
        manageLocations,
        companionRequest,
        addCompanion,
        removeCompanion,
        attachCompanions,
        cancelCompanions,
        hardDiskDirectory,
        finishHardDisk,
        cancelHardDisk,
        workspaceManagerOpen,
        activeWorkspaceId,
        closeWorkspaceManager,
        connectionSettings,
        saveRemoteConnection,
        switchToLocalConnection,
        closeConnectionSettings,
        mutation,
        packageImport,
        packageBatchImport,
        exports,
        volumePackages,
        volumeFloppies,
        mediaExports,
        deletion,
        programGeneration,
        mediaDrop,
        audioImport,
        audioFileInput,
        sequenceImport,
        tx16wImport,
        sequenceFileInput,
        sampleNames,
        sampleBankNames,
        waveDataNames,
        sequenceNames,
    }: Props = $props();

    function directChoiceVisible(operation: DirectComputerOperation, contentAvailable: boolean): boolean {
        return directComputerDialogVisible(directComputerOperation, operation, contentAvailable);
    }

    const exportProgressVisibility = new DelayedExportProgressVisibility();

    function busyExportOperation(): ExportProgressOperation | null {
        if (exports.packageRequest?.busy) return 'package-export';
        if (volumePackages.request?.busy) return 'volume-package-export';
        if (volumeFloppies.request?.busy) return 'volume-floppy-export';
        if (exports.audioRequest?.busy) return 'audio-export';
        if (exports.sequenceRequest?.busy) return 'sequence-export';
        if (mediaExports.request?.busy) return 'media-export';
        return null;
    }

    $effect(() => exportProgressVisibility.update(busyExportOperation()));
    onDestroy(() => exportProgressVisibility.dispose());
</script>

{#if pickerRequest}
    <ServerStoragePicker
        {transport}
        mode={pickerRequest.mode}
        title={pickerRequest.title}
        extensions={pickerRequest.extensions}
        suggestedName={pickerRequest.suggestedName}
        multiple={pickerRequest.multiple}
        initialDirectory={pickerRequest.initialDirectory}
        initialFile={pickerRequest.initialFile}
        requireWritableDirectory={pickerRequest.requireWritableDirectory}
        ondirectorychange={pickerRequest.ondirectorychange}
        onmanagelocations={() => {
            finishPicker(null);
            manageLocations();
        }}
        onselect={(selection) => finishPicker(selection)}
        onselectmany={(selections) => finishPicker(selections)}
        oncancel={() => finishPicker(null)}
    />
{/if}
{#if companionRequest && pickerRequest?.parentDialog !== 'companion-disks'}
    <CompanionDiskDialog
        sources={companionRequest.sources}
        sourceKind={companionRequest.sourceKind}
        setLabel={companionRequest.setLabel}
        nextRequiredIndex={companionRequest.nextRequiredIndex}
        busy={companionRequest.busy}
        error={companionRequest.error}
        onadd={addCompanion}
        onremove={removeCompanion}
        onnearby={() => attachCompanions({ kind: 'immediate-siblings' })}
        onconfirm={() => attachCompanions({ kind: 'sources', sources: companionRequest.sources })}
        oncancel={cancelCompanions}
    />
{/if}
{#if hardDiskDirectory}
    <CreateHardDiskImageDialog
        {transport}
        directory={hardDiskDirectory}
        onsuccess={finishHardDisk}
        oncancel={cancelHardDisk}
    />
{/if}
<WorkspaceManager open={workspaceManagerOpen} {activeWorkspaceId} onclose={closeWorkspaceManager} />
{#if connectionSettings}
    <ServerConnectionSettings
        settings={connectionSettings}
        onsave={saveRemoteConnection}
        onuselocal={switchToLocalConnection}
        oncancel={closeConnectionSettings}
    />
{/if}
{#if mutation.volumeAction}
    {#key `${mutation.volumeAction.action}:${mutation.volumeAction.item.id}`}
        <VolumeActionDialog
            action={mutation.volumeAction.action}
            item={mutation.volumeAction.item}
            busy={mutation.volumeActionBusy}
            phase={mutation.volumeActionPhase}
            error={mutation.volumeActionError}
            deletionInspection={mutation.volumeDeletionInspection}
            oncancel={() => mutation.cancelVolumeAction()}
            onsubmit={(name) => void mutation.submitVolumeAction(name)}
        />
    {/key}
{/if}
{#if mutation.placementRepairRequest}
    {#key mutation.placementRepairRequest.item.id}
        <PlacementRepairDialog
            item={mutation.placementRepairRequest.item}
            inspection={mutation.placementRepairRequest.inspection}
            busy={mutation.placementRepairRequest.busy}
            phase={mutation.placementRepairRequest.phase}
            error={mutation.placementRepairRequest.error}
            message={mutation.placementRepairRequest.message}
            oncancel={() => mutation.cancelPlacementRepair()}
            onsubmit={(recoveryVolumeName) => void mutation.submitPlacementRepair(recoveryVolumeName)}
        />
    {/key}
{/if}
{#if mutation.objectRenameRequest}
    {#key mutation.objectRenameRequest.target.object.key}
        <ObjectRenameDialog
            target={mutation.objectRenameRequest.target}
            busy={mutation.objectRenameRequest.busy}
            error={mutation.objectRenameRequest.error}
            oncancel={() => mutation.cancelObjectRename()}
            onsubmit={(name) => void mutation.submitObjectRename(name)}
        />
    {/key}
{/if}
{#if mutation.sampleBankCreationRequest}
    <CreateSampleBankDialog
        volumeName={mutation.sampleBankCreationRequest.volumeName}
        sampleCount={mutation.sampleBankCreationRequest.samples.length}
        assignedSampleCount={mutation.sampleBankCreationRequest.assignedSampleCount}
        existingNames={mutation.sampleBankCreationRequest.existingNames}
        busy={mutation.sampleBankCreationRequest.busy}
        error={mutation.sampleBankCreationRequest.error}
        oncancel={() => mutation.cancelSampleBankCreation()}
        onsubmit={(name) => void mutation.submitSampleBankCreation(name)}
    />
{/if}
{#if mutation.sampleBankAssignmentRequest}
    <AssignSampleBankDialog
        volumeName={mutation.sampleBankAssignmentRequest.volumeName}
        sampleCount={mutation.sampleBankAssignmentRequest.samples.length}
        options={mutation.sampleBankAssignmentRequest.options}
        blockers={mutation.sampleBankAssignmentRequest.blockers}
        busy={mutation.sampleBankAssignmentRequest.busy}
        error={mutation.sampleBankAssignmentRequest.error}
        oncancel={() => mutation.cancelSampleBankAssignment()}
        onsubmit={(bankObjectId) => void mutation.submitSampleBankAssignment(bankObjectId)}
    />
{/if}
{#if packageImport.request && pickerRequest?.parentDialog !== 'package-import' && directChoiceVisible('package-import', packageImport.request.status !== 'choosing' || Boolean(packageImport.request.sourceName || packageImport.request.error))}
    <PackageImportDialog
        targetName={packageImport.targetName()}
        destinationMode={packageImport.request.destinationMode}
        destinationPartitionIndex={packageImport.request.destinationPartitionIndex}
        destinationVolumeName={packageImport.request.destinationVolumeName}
        partitionOptions={packageImport.partitionOptions()}
        volumeOptions={packageImport.volumeOptions()}
        desktop={isDesktop}
        canChangeSource={packageImport.request.canChangeSource}
        sourceName={packageImport.request.sourceName}
        inspection={packageImport.request.inspection}
        plan={packageImport.request.plan}
        renames={packageImport.request.renames}
        programSlots={packageImport.request.programSlots}
        opaqueSequenceActions={packageImport.request.opaqueSequenceActions}
        hasUnvalidatedChanges={packageImport.request.hasUnvalidatedChanges}
        status={packageImport.request.status}
        progress={packageImport.request.progress}
        error={packageImport.request.error}
        onchooseworkspace={() => void packageImport.chooseWorkspace()}
        onchooselocal={() => void packageImport.chooseLocal()}
        onchange={() => void packageImport.resetSource()}
        ondestinationmode={(mode) => packageImport.setDestinationMode(mode)}
        ondestinationvolume={(partitionIndex, volumeName) =>
            packageImport.setExistingVolume(partitionIndex, volumeName)}
        ondestinationpartition={(partitionIndex) => packageImport.setDestinationPartition(partitionIndex)}
        ondestinationname={(name) => packageImport.setDestinationVolumeName(name)}
        onrename={(nodeId, name) => packageImport.rename(nodeId, name)}
        onprogramslot={(nodeId, slot) => packageImport.programSlot(nodeId, slot)}
        onprogramstart={(placementId, start) => packageImport.programStart(placementId, start)}
        onopaquesequenceaction={(nodeId, action) => packageImport.opaqueSequenceAction(nodeId, action)}
        onreplan={() => void packageImport.replan()}
        oncancel={() => void packageImport.close()}
        onconfirm={() => void packageImport.apply()}
    />
{/if}
{#if packageBatchImport.request && pickerRequest?.parentDialog !== 'package-import' && directChoiceVisible('package-batch-import', packageBatchImport.request.status !== 'choosing' || packageBatchImport.request.items.length > 0 || Boolean(packageBatchImport.request.error))}
    <PackageBatchImportDialog
        partitionName={packageBatchImport.request.partition.name}
        desktop={isDesktop}
        items={packageBatchImport.request.items}
        plan={packageBatchImport.request.plan}
        volumeNames={packageBatchImport.request.volumeNames}
        opaqueSequenceActions={packageBatchImport.request.opaqueSequenceActions}
        hasUnvalidatedChanges={packageBatchImport.request.hasUnvalidatedChanges}
        status={packageBatchImport.request.status}
        completedFiles={packageBatchImport.request.completedFiles}
        totalFiles={packageBatchImport.request.totalFiles}
        progress={packageBatchImport.request.progress}
        error={packageBatchImport.request.error}
        onchooseworkspace={() => void packageBatchImport.chooseWorkspace()}
        onchooselocal={() => void packageBatchImport.chooseLocal()}
        onrename={(itemId, name) => packageBatchImport.renameVolume(itemId, name)}
        ontoggleselected={(itemId, selected) => packageBatchImport.setSelected(itemId, selected)}
        ontoggleall={(selected) => packageBatchImport.setAllSelected(selected)}
        onopaquesequenceaction={(itemId, nodeId, action) =>
            packageBatchImport.opaqueSequenceAction(itemId, nodeId, action)}
        onreplan={() => packageBatchImport.replan()}
        oncancel={() => void packageBatchImport.close()}
        onconfirm={() => void packageBatchImport.apply()}
    />
{/if}
{#if exports.packageRequest && !exports.packageRequest.busy && pickerRequest?.parentDialog !== 'package-export' && !companionRequest && directChoiceVisible('package-export', Boolean(exports.packageRequest.error))}
    <PackageExportDialog
        items={exports.packageRequest.items}
        desktop={isDesktop}
        error={exports.packageRequest.error}
        onworkspace={() => void exports.packageToWorkspace()}
        onlocal={() => void exports.packageToComputer()}
        oncancel={() => exports.closePackage()}
    />
{/if}
{#if volumePackages.request && !volumePackages.request.busy && pickerRequest?.parentDialog !== 'volume-package-export' && directChoiceVisible('volume-package-export', Boolean(volumePackages.request.error))}
    <VolumePackageExportDialog
        scopeName={volumePackages.request.scope.name}
        inspection={volumePackages.request.inspection}
        desktop={isDesktop}
        loading={volumePackages.request.loading}
        error={volumePackages.request.error}
        onworkspace={() => void volumePackages.toWorkspace()}
        onlocal={() => void volumePackages.toComputer()}
        oncancel={() => volumePackages.cancel()}
    />
{/if}
{#if volumeFloppies.request && !volumeFloppies.request.busy && pickerRequest?.parentDialog !== 'volume-floppy-export' && directChoiceVisible('volume-floppy-export', Boolean(volumeFloppies.request.error))}
    <VolumeFloppyExportDialog
        scopeName={volumeFloppies.request.scope.name}
        inspection={volumeFloppies.request.inspection}
        desktop={isDesktop}
        loading={volumeFloppies.request.loading}
        error={volumeFloppies.request.error}
        onworkspace={() => void volumeFloppies.toWorkspace()}
        onlocal={() => void volumeFloppies.toComputer()}
        oncancel={() => volumeFloppies.cancel()}
    />
{/if}
{#if exports.audioRequest && !exports.audioRequest.busy && pickerRequest?.parentDialog !== 'audio-export' && !companionRequest && directChoiceVisible('audio-export', Boolean(exports.audioRequest.error))}
    <SfzExportDialog
        items={exports.audioRequest.items}
        inspection={exports.audioRequest.inspection}
        desktop={isDesktop}
        loading={exports.audioRequest.loading}
        error={exports.audioRequest.error}
        format={exports.audioRequest.format}
        onformatchange={(format) => exports.setAudioFormat(format)}
        onworkspace={() => void exports.audioToWorkspace()}
        onlocal={() => void exports.audioToComputer()}
        oncancel={() => exports.cancelAudio()}
    />
{/if}
{#if exports.sequenceRequest && !exports.sequenceRequest.busy && pickerRequest?.parentDialog !== 'sequence-export' && directChoiceVisible('sequence-export', Boolean(exports.sequenceRequest.error))}
    <MidiExportDialog
        items={exports.sequenceRequest.items}
        desktop={isDesktop}
        error={exports.sequenceRequest.error}
        onworkspace={() => void exports.sequenceToWorkspace()}
        onlocal={() => void exports.sequenceToComputer()}
        oncancel={() => exports.cancelSequence()}
    />
{/if}
{#if mediaExports.request && !mediaExports.request.busy && pickerRequest?.parentDialog !== 'media-export' && directChoiceVisible('media-export', Boolean(mediaExports.request.error))}
    <MediaExportDialog
        request={mediaExports.request}
        desktop={isDesktop}
        onworkspace={() => void mediaExports.toWorkspace()}
        onlocal={() => void mediaExports.toComputer()}
        oncancel={() => mediaExports.cancel()}
    />
{/if}
{#if exportProgressVisibility.operation === 'package-export' && exports.packageRequest?.busy}
    <ExportProgressDialog
        title="Export package"
        progressLabel={exports.packageRequest.progressLabel || 'Exporting package…'}
        cancellable={false}
        oncancel={() => undefined}
    />
{:else if exportProgressVisibility.operation === 'volume-package-export' && volumePackages.request?.busy}
    <ExportProgressDialog
        title="Export volume packages"
        progressLabel={volumePackages.request.progressLabel || 'Exporting packages…'}
        cancellable={true}
        oncancel={() => volumePackages.cancel()}
    />
{:else if exportProgressVisibility.operation === 'volume-floppy-export' && volumeFloppies.request?.busy}
    <ExportProgressDialog
        title="Export volumes to floppies"
        progressLabel={volumeFloppies.request.progressLabel || 'Exporting floppy sets…'}
        cancellable={true}
        oncancel={() => volumeFloppies.cancel()}
    />
{:else if exportProgressVisibility.operation === 'audio-export' && exports.audioRequest?.busy}
    <ExportProgressDialog
        title="Export audio"
        progressLabel={exports.audioRequest.progressLabel || 'Exporting audio…'}
        cancellable={true}
        oncancel={() => exports.cancelAudio()}
    />
{:else if exportProgressVisibility.operation === 'sequence-export' && exports.sequenceRequest?.busy}
    <ExportProgressDialog
        title="Export MIDI"
        progressLabel={exports.sequenceRequest.progressLabel || 'Exporting MIDI…'}
        cancellable={true}
        oncancel={() => exports.cancelSequence()}
    />
{:else if exportProgressVisibility.operation === 'media-export' && mediaExports.request?.busy}
    <ExportProgressDialog
        title="Export sampler media"
        progressLabel={mediaExports.request.progressLabel || 'Exporting sampler media…'}
        cancellable={true}
        oncancel={() => mediaExports.cancel()}
    />
{/if}
{#if deletion.objectRequest}
    <ObjectDeletionDialog
        inspection={deletion.objectRequest.inspection}
        loading={deletion.objectRequest.loading}
        busy={deletion.objectRequest.busy}
        error={deletion.objectRequest.error}
        onselectionchange={(objectId, selected) => deletion.updateObjectSelection(objectId, selected)}
        onselectall={(selected) => deletion.updateAllObjectDependencies(selected)}
        oncancel={() => deletion.cancelObjects()}
        onconfirm={() => void deletion.submitObjects()}
    />
{/if}
{#if deletion.cleanupRequest}
    <WaveDataCleanupDialog
        volumeName={deletion.cleanupRequest.volumeName}
        inspection={deletion.cleanupRequest.inspection}
        selectedObjectIds={deletion.cleanupRequest.selectedObjectIds}
        loading={deletion.cleanupRequest.loading}
        busy={deletion.cleanupRequest.busy}
        error={deletion.cleanupRequest.error}
        onselectionchange={(objectId, selected) => deletion.updateCleanupSelection(objectId, selected)}
        onselectall={(selected) => deletion.updateAllCleanup(selected)}
        oncancel={() => deletion.cancelCleanup()}
        onconfirm={() => void deletion.submitCleanup()}
    />
{/if}
{#if programGeneration.request}
    <ProgramGenerationDialog
        volumeName={programGeneration.request.volumeName}
        inspection={programGeneration.request.inspection}
        rows={programGeneration.request.rows}
        loading={programGeneration.request.loading}
        busy={programGeneration.request.busy}
        error={programGeneration.request.error}
        onselectionchange={(objectId, selected) => programGeneration.setSelected(objectId, selected)}
        onnamechange={(objectId, name) => programGeneration.setProgramName(objectId, name)}
        onselectall={(selected) => programGeneration.setAllSelected(selected)}
        oncancel={() => programGeneration.close()}
        onconfirm={() => void programGeneration.submit()}
    />
{/if}
{#if audioImport.request && pickerRequest?.parentDialog !== 'audio-import' && directChoiceVisible('audio-import', audioImport.request.files.length > 0)}
    <AudioImportDialog
        {transport}
        files={audioImport.request.files}
        target={audioImport.destination()}
        destinationMode={audioImport.request.destinationMode}
        destinationPartitionIndex={audioImport.request.destinationPartitionIndex}
        destinationVolumeName={audioImport.request.destinationVolumeName}
        partitionOptions={audioImport.partitionOptions()}
        volumeOptions={audioImport.volumeOptions()}
        existingSampleNames={audioImport.request.destinationMode === 'existing' ? sampleNames : []}
        existingSampleBankNames={audioImport.request.destinationMode === 'existing' ? sampleBankNames : []}
        existingWaveformNames={audioImport.request.destinationMode === 'existing' ? waveDataNames : []}
        onchooseworkspace={() => void audioImport.chooseWorkspace()}
        onchooselocal={transport.supportsClientUploads && audioFileInput
            ? () => audioImport.chooseLocal(audioFileInput)
            : undefined}
        ondestinationmode={(mode) => audioImport.setDestinationMode(mode)}
        ondestinationvolume={(partitionIndex, volumeName) =>
            void audioImport.setExistingVolume(partitionIndex, volumeName)}
        ondestinationpartition={(partitionIndex) => audioImport.setDestinationPartition(partitionIndex)}
        ondestinationname={(volumeName) => audioImport.setDestinationVolumeName(volumeName)}
        oncommit={(items, grouping) => audioImport.commit(items, grouping)}
        oncancel={() => (audioImport.request = null)}
    />
{/if}
{#if sequenceImport.request && pickerRequest?.parentDialog !== 'sequence-import' && directChoiceVisible('sequence-import', sequenceImport.request.files.length > 0)}
    <MidiImportDialog
        {transport}
        files={sequenceImport.request.files}
        target={sequenceImport.request.target}
        existingSequenceNames={sequenceNames}
        onchooseworkspace={() => void sequenceImport.chooseWorkspace()}
        onchooselocal={transport.supportsClientUploads && sequenceFileInput
            ? () => sequenceImport.chooseLocal(sequenceFileInput)
            : undefined}
        oncommit={(items, systemExclusivePolicy) => sequenceImport.commit(items, systemExclusivePolicy)}
        oncancel={() => (sequenceImport.request = null)}
    />
{/if}
{#if tx16wImport.request}
    <Tx16wImportDialog
        request={tx16wImport.request}
        volumeOptions={tx16wImport.volumeOptions()}
        ontarget={(target) => void tx16wImport.selectTarget(target)}
        onmode={(mode) => void tx16wImport.selectImportMode(mode)}
        onadd={(files) => void tx16wImport.addFiles(files)}
        onremove={(memberId) => void tx16wImport.removeMember(memberId)}
        onconfirm={() => void tx16wImport.commit()}
        oncancel={() => void tx16wImport.close()}
    />
{/if}
{#if mediaDrop.notice}
    <ImportUnavailableDialog
        title={mediaDrop.notice.title}
        message={mediaDrop.notice.message}
        onclose={() => mediaDrop.closeNotice()}
    />
{/if}
{#if mediaDrop.dragActive && !audioImport.request && !sequenceImport.request && !tx16wImport.request && !packageImport.request}
    <div class="media-drop-overlay" aria-hidden="true">
        <Icon name="upload" size={24} />
        {#if mediaDrop.dragKind === 'mixed'}
            <strong>Drop one media type at a time</strong>
            <span>Use one media type per drop</span>
        {:else if mediaDrop.dragKind === 'tx16w'}
            <strong
                >{mediaDrop.dragTarget
                    ? `Import TX16W disk set into ${mediaDrop.dragTarget.volumeName}`
                    : 'Choose a target volume after dropping'}</strong
            >
            <span>One or more TX16W Yamaha-format disk images (.img, .ima)</span>
        {:else if mediaDrop.dragKind === 'midi'}
            <strong
                >{mediaDrop.dragTarget
                    ? `Import MIDI into ${mediaDrop.dragTarget.volumeName}`
                    : 'Select a writable volume'}</strong
            >
            <span>Standard MIDI Files (.mid, .midi)</span>
        {:else if mediaDrop.dragKind === 'package'}
            <strong>Import package or A3K archive</strong>
            <span>Choose an existing volume or create a new one after dropping</span>
        {:else}
            <strong
                >{mediaDrop.dragTarget
                    ? `Import audio into ${mediaDrop.dragTarget.volumeName}`
                    : 'Choose a target volume after dropping'}</strong
            >
            <span>WAV, FLAC, and AIFF</span>
        {/if}
    </div>
{/if}
