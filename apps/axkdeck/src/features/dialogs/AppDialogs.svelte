<script lang="ts">
    import type { DeletionWorkflow } from '../deletion/workflow.svelte';
    import type { PickerRequest, PickerSelection } from './picker';
    import type { ExportWorkflow } from '../export/workflow.svelte';
    import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
    import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
    import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
    import type { AudioImportWorkflow } from '../import/audioWorkflow.svelte';
    import type { MediaDropWorkflow } from '../import/mediaDropWorkflow.svelte';
    import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
    import type { SequenceImportWorkflow } from '../import/sequenceWorkflow.svelte';
    import type { MutationWorkflow } from '../mutation/workflow.svelte';
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
    import PackageImportDialog from '../../lib/components/PackageImportDialog.svelte';
    import PlacementRepairDialog from '../../lib/components/PlacementRepairDialog.svelte';
    import MidiExportDialog from '../../lib/components/MidiExportDialog.svelte';
    import MidiImportDialog from '../../lib/components/MidiImportDialog.svelte';
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
        exports: ExportWorkflow;
        volumePackages: VolumePackageExportWorkflow;
        volumeFloppies: VolumeFloppyExportWorkflow;
        mediaExports: MediaExportWorkflow;
        deletion: DeletionWorkflow;
        mediaDrop: MediaDropWorkflow;
        audioImport: AudioImportWorkflow;
        audioFileInput?: HTMLInputElement;
        sequenceImport: SequenceImportWorkflow;
        sequenceFileInput?: HTMLInputElement;
        sampleNames: string[];
        sampleBankNames: string[];
        waveDataNames: string[];
        sequenceNames: string[];
    }

    let {
        transport,
        isDesktop,
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
        exports,
        volumePackages,
        volumeFloppies,
        mediaExports,
        deletion,
        mediaDrop,
        audioImport,
        audioFileInput,
        sequenceImport,
        sequenceFileInput,
        sampleNames,
        sampleBankNames,
        waveDataNames,
        sequenceNames,
    }: Props = $props();
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
{#if packageImport.request && pickerRequest?.parentDialog !== 'package-import'}
    <PackageImportDialog
        targetName={packageImport.request.item.name}
        desktop={isDesktop}
        sourceName={packageImport.request.sourceName}
        inspection={packageImport.request.inspection}
        plan={packageImport.request.plan}
        renames={packageImport.request.renames}
        programSlots={packageImport.request.programSlots}
        status={packageImport.request.status}
        progress={packageImport.request.progress}
        error={packageImport.request.error}
        onchooseworkspace={() => void packageImport.chooseWorkspace()}
        onchooselocal={() => void packageImport.chooseLocal()}
        onchange={() => void packageImport.resetSource()}
        onrename={(nodeId, name) => packageImport.rename(nodeId, name)}
        onprogramslot={(nodeId, slot) => packageImport.programSlot(nodeId, slot)}
        onprogramstart={(placementId, start) => packageImport.programStart(placementId, start)}
        onreplan={() => void packageImport.replan()}
        oncancel={() => void packageImport.close()}
        onconfirm={() => void packageImport.apply()}
    />
{/if}
{#if exports.packageRequest && pickerRequest?.parentDialog !== 'package-export' && !companionRequest}
    <PackageExportDialog
        items={exports.packageRequest.items}
        desktop={isDesktop}
        busy={exports.packageRequest.busy}
        progressLabel={exports.packageRequest.progressLabel}
        error={exports.packageRequest.error}
        onworkspace={() => void exports.packageToWorkspace()}
        onlocal={() => void exports.packageToComputer()}
        oncancel={() => exports.closePackage()}
    />
{/if}
{#if volumePackages.request && pickerRequest?.parentDialog !== 'volume-package-export'}
    <VolumePackageExportDialog
        scopeName={volumePackages.request.scope.name}
        inspection={volumePackages.request.inspection}
        desktop={isDesktop}
        loading={volumePackages.request.loading}
        busy={volumePackages.request.busy}
        progressLabel={volumePackages.request.progressLabel}
        error={volumePackages.request.error}
        onworkspace={() => void volumePackages.toWorkspace()}
        onlocal={() => void volumePackages.toComputer()}
        oncancel={() => volumePackages.cancel()}
    />
{/if}
{#if volumeFloppies.request && pickerRequest?.parentDialog !== 'volume-floppy-export'}
    <VolumeFloppyExportDialog
        scopeName={volumeFloppies.request.scope.name}
        inspection={volumeFloppies.request.inspection}
        desktop={isDesktop}
        loading={volumeFloppies.request.loading}
        busy={volumeFloppies.request.busy}
        progressLabel={volumeFloppies.request.progressLabel}
        error={volumeFloppies.request.error}
        onworkspace={() => void volumeFloppies.toWorkspace()}
        onlocal={() => void volumeFloppies.toComputer()}
        oncancel={() => volumeFloppies.cancel()}
    />
{/if}
{#if exports.audioRequest && pickerRequest?.parentDialog !== 'audio-export' && !companionRequest}
    <SfzExportDialog
        items={exports.audioRequest.items}
        inspection={exports.audioRequest.inspection}
        desktop={isDesktop}
        loading={exports.audioRequest.loading}
        busy={exports.audioRequest.busy}
        progressLabel={exports.audioRequest.progressLabel}
        error={exports.audioRequest.error}
        format={exports.audioRequest.format}
        onformatchange={(format) => exports.setAudioFormat(format)}
        onworkspace={() => void exports.audioToWorkspace()}
        onlocal={() => void exports.audioToComputer()}
        oncancel={() => exports.cancelAudio()}
    />
{/if}
{#if exports.sequenceRequest && pickerRequest?.parentDialog !== 'sequence-export'}
    <MidiExportDialog
        items={exports.sequenceRequest.items}
        desktop={isDesktop}
        busy={exports.sequenceRequest.busy}
        progressLabel={exports.sequenceRequest.progressLabel}
        error={exports.sequenceRequest.error}
        onworkspace={() => void exports.sequenceToWorkspace()}
        onlocal={() => void exports.sequenceToComputer()}
        oncancel={() => exports.cancelSequence()}
    />
{/if}
{#if mediaExports.request && pickerRequest?.parentDialog !== 'media-export'}
    <MediaExportDialog
        request={mediaExports.request}
        desktop={isDesktop}
        onworkspace={() => void mediaExports.toWorkspace()}
        onlocal={() => void mediaExports.toComputer()}
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
{#if audioImport.request && pickerRequest?.parentDialog !== 'audio-import'}
    <AudioImportDialog
        {transport}
        files={audioImport.request.files}
        target={audioImport.request.target}
        existingSampleNames={sampleNames}
        existingSampleBankNames={sampleBankNames}
        existingWaveformNames={waveDataNames}
        onchooseworkspace={() => void audioImport.chooseWorkspace()}
        onchooselocal={transport.supportsClientUploads && audioFileInput
            ? () => audioImport.chooseLocal(audioFileInput)
            : undefined}
        oncommit={(items, grouping) => audioImport.commit(items, grouping)}
        oncancel={() => (audioImport.request = null)}
    />
{/if}
{#if sequenceImport.request && pickerRequest?.parentDialog !== 'sequence-import'}
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
{#if mediaDrop.notice}
    <ImportUnavailableDialog
        title={mediaDrop.notice.title}
        message={mediaDrop.notice.message}
        onclose={() => mediaDrop.closeNotice()}
    />
{/if}
{#if mediaDrop.dragActive && !audioImport.request && !sequenceImport.request}
    <div class="media-drop-overlay" aria-hidden="true">
        <Icon name="upload" size={24} />
        {#if mediaDrop.dragKind === 'mixed'}
            <strong>Drop audio and MIDI files separately</strong>
            <span>Use one media type per drop</span>
        {:else if mediaDrop.dragKind === 'midi'}
            <strong
                >{mediaDrop.dragTarget
                    ? `Import MIDI into ${mediaDrop.dragTarget.volumeName}`
                    : 'Select a writable volume'}</strong
            >
            <span>Standard MIDI Files (.mid, .midi)</span>
        {:else}
            <strong
                >{mediaDrop.dragTarget
                    ? `Import audio into ${mediaDrop.dragTarget.volumeName}`
                    : 'Select a writable volume'}</strong
            >
            <span>WAV, FLAC, and AIFF</span>
        {/if}
    </div>
{/if}
