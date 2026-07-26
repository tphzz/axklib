<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import AuditionBar from './lib/components/AuditionBar.svelte';
    import ContainedObjectWorkspace from './lib/components/ContainedObjectWorkspace.svelte';
    import Icon from './lib/components/Icon.svelte';
    import ImageNavigator from './lib/components/ImageNavigator.svelte';
    import LayoutControls from './lib/components/LayoutControls.svelte';
    import ObjectInspector from './lib/components/ObjectInspector.svelte';
    import ObjectEditor from './lib/components/ObjectEditor.svelte';
    import ObjectDeletionDialog from './lib/components/ObjectDeletionDialog.svelte';
    import WaveDataCleanupDialog from './lib/components/WaveDataCleanupDialog.svelte';
    import ObjectRenameDialog from './lib/components/ObjectRenameDialog.svelte';
    import PackageExportDialog from './lib/components/PackageExportDialog.svelte';
    import PackageImportDialog from './lib/components/PackageImportDialog.svelte';
    import PackageSelectionControls from './lib/components/PackageSelectionControls.svelte';
    import ObjectWorkspace from './lib/components/ObjectWorkspace.svelte';
    import ServerConnectionSettings from './lib/components/ServerConnectionSettings.svelte';
    import CreateHardDiskImageDialog from './lib/components/CreateHardDiskImageDialog.svelte';
    import AudioImportDialog from './lib/components/AudioImportDialog.svelte';
    import CompanionDiskDialog from './lib/components/CompanionDiskDialog.svelte';
    import VolumeActionDialog from './lib/components/VolumeActionDialog.svelte';
    import ServerStoragePicker from './lib/components/ServerStoragePicker.svelte';
    import WorkspaceManager from './lib/components/WorkspaceManager.svelte';
    import { AuditionController, type AuditionState } from './lib/audio/auditionController';
    import { inspectorSelectionStopsPlayback } from './lib/audio/playbackSelection';
    import { matchesSearch, playbackRowVisible } from './lib/auditionVisibility';
    import { createTransport } from './lib/createTransport';
    import { objectPresentationName } from './lib/objectPresentation';
    import {
        auditionableSampleIds,
        distinctWaveDataForSample,
        linkedWaveDataForSample,
        orderedSamplesForBank,
    } from './lib/sampleRelationships';
    import {
        configureRemoteServer,
        remoteServerSettings,
        useLocalServer,
        type RemoteServerSettingsInput,
        type RemoteServerSettingsView,
    } from './lib/serverSettings';
    import { audioExtensions, audioMediaType } from './lib/audioImport';
    import { browserUploadSource, type ClientUploadSource } from './lib/clientUploadSource';
    import { diagnosticsEnabled, reportDiagnostic, reportError } from './lib/diagnostics';
    import { listenForNativeAudioDrops, type NativeDropPosition } from './lib/nativeAudioDrop';
    import { nativeFileSource } from './lib/nativeFileSource';
    import { saveRetainedPackage, selectLocalPackage, selectLocalPackageDestination } from './lib/nativePackages';
    import { collectPages } from './lib/pagination';
    import {
        emptyPackageExportSelection,
        maximumPackageExportRoots,
        type PackageExportSelectionState,
    } from './lib/objectSelection';
    import { packageExportFilename } from './lib/packageExport';
    import {
        type ClientUploadLocation,
        type DirectoryLocation,
        type DirectoryRef,
        type FileLocation,
        type ImageLocation,
        type InputFileLocation,
    } from './lib/storageLocations';
    import type {
        AudioImportItem,
        AudioImportTarget,
        CompanionDirectorySelection,
        ObjectDeletionInspection,
        WaveDataOrphanInspection,
        ImageSessionPackageExportResult,
        ImageSessionPackageExportRoot,
        ImageSessionPackageImportPlan,
        PackageInspection,
        ObjectRenameMutation,
        OpenedImage,
        SamplerObject,
        SamplerRelationship,
        PartitionMutation,
        VolumeMutation,
    } from './lib/transport';
    import { userFacingMessage } from './lib/userFacingMessage';
    import type { InterfaceScaleController, InterfaceScaleMode, InterfaceScaleState } from './lib/interfaceScale';
    import type {
        DiskTreeItem,
        InspectorSelection,
        ObjectRenameTarget,
        PackageExportObject,
        PackageExportSelection,
        Program,
        ProgramAssignmentRow,
        SampleStructureItem,
        SampleWaveformPreview,
        WaveDataItem,
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
        icon: 'music' | 'layers' | 'archive' | 'waveform';
    }[] = [
        { id: 'programs', label: 'Programs', icon: 'music' },
        { id: 'sample-banks', label: 'Sample Banks', icon: 'layers' },
        { id: 'samples', label: 'Samples', icon: 'archive' },
        { id: 'wave-data', label: 'Wave Data', icon: 'waveform' },
    ];
    const packageExtensions = ['axkvol', 'axkprg', 'axksbac', 'axksbnk', 'axksmpl', 'axkpkg'];
    const packageExtensionSet = new Set(packageExtensions);

    const transport = createTransport();
    const isDesktop = '__TAURI_INTERNALS__' in window;
    type PickerMode = 'file' | 'directory' | 'save-file' | 'media-source';
    type PickerParentDialog = 'audio-import' | 'companion-disks' | 'package-import' | 'package-export';
    type PickerSelection = ImageLocation | DirectoryLocation | FileLocation[];
    interface PickerRequest {
        mode: PickerMode;
        title: string;
        extensions: string[];
        suggestedName: string;
        multiple: boolean;
        parentDialog?: PickerParentDialog;
        requireWritableDirectory?: boolean;
        initialDirectory?: DirectoryRef | null;
        ondirectorychange?: (directory: DirectoryRef | null) => void;
        resolve: (selection: PickerSelection | null) => void;
    }

    interface LaneQueries {
        primary: string;
        secondary: string;
        tertiary: string;
    }

    let sourceItems = $state<DiskTreeItem[]>([]);
    let selectedSource = $state<DiskTreeItem>({ id: 'none', name: 'No image', kind: 'disk', childCount: 0 });
    let imageLocation = $state<ImageLocation | null>(null);
    let pickerRequest = $state<PickerRequest | null>(null);
    let hardDiskCreationDirectory = $state<DirectoryLocation | null>(null);
    let lastImageDirectory = $state<DirectoryRef | null>(null);
    let lastPackageDirectory = $state<DirectoryRef | null>(null);
    let lastAudioDirectory = $state<DirectoryRef | null>(null);
    let lastCompanionDirectory = $state<DirectoryRef | null>(null);
    let openSessionId = $state<number | null>(null);
    let companionDirectories = $state<DirectoryRef[]>([]);
    let imageOpening = $state(false);
    let imageOpenGeneration = 0;
    let sourceStatus = $state('Ready');
    let sourceObjectCount = $state(0);
    let workspaceView = $state<WorkspaceView>('programs');
    let programs = $state<Program[]>([]);
    let sampleBanks = $state<SampleStructureItem[]>([]);
    let samples = $state<SampleStructureItem[]>([]);
    let waveData = $state<WaveDataItem[]>([]);
    let relationships = $state<SamplerRelationship[]>([]);
    let objectsById = $state(new Map<string, SamplerObject>());
    let selectedProgramId = $state('');
    let selectedBankId = $state('');
    let selectedBankMemberId = $state('');
    let selectedSampleId = $state('');
    let selectedBankWaveDataId = $state('');
    let selectedSampleWaveDataId = $state('');
    let selectedWaveDataId = $state('');
    let inspectorObjectId = $state('');
    let editorObjectIds = $state<Record<WorkspaceView, string>>({
        programs: '',
        'sample-banks': '',
        samples: '',
        'wave-data': '',
    });
    let laneQueries = $state<Record<WorkspaceView, LaneQueries>>({
        programs: { primary: '', secondary: '', tertiary: '' },
        'sample-banks': { primary: '', secondary: '', tertiary: '' },
        samples: { primary: '', secondary: '', tertiary: '' },
        'wave-data': { primary: '', secondary: '', tertiary: '' },
    });
    let sidebarOpen = $state(true);
    let lowerPanelOpen = $state(false);
    let inspectorOpen = $state(true);
    let splitRatio = $state(2 / 3);
    let mainStage: HTMLElement;
    let resizing = $state(false);
    let connectionSettings = $state<RemoteServerSettingsView | null>(null);
    let workspaceManagerOpen = $state(false);
    let volumeMutationsAvailable = $state(false);
    let partitionMutationsAvailable = $state(false);
    let objectRenameAvailable = $state(false);
    let objectDeletionAvailable = $state(false);
    let waveDataCleanupAvailable = $state(false);
    let packageImportAvailable = $state(false);
    let packageExportAvailable = $state(false);
    let volumeAction = $state<{ item: DiskTreeItem; action: ImageTreeAction } | null>(null);
    let volumeActionBusy = $state(false);
    let volumeActionError = $state('');
    let objectRenameRequest = $state<{
        target: ObjectRenameTarget;
        busy: boolean;
        error: string;
    } | null>(null);
    let objectDeletionRequest = $state<{
        targets: PackageExportObject[];
        cleanupObjectIds: string[];
        inspection: ObjectDeletionInspection | null;
        loading: boolean;
        busy: boolean;
        error: string;
    } | null>(null);
    let objectDeletionGeneration = 0;
    let waveDataCleanupRequest = $state<{
        volumeId: string;
        volumeName: string;
        inspection: WaveDataOrphanInspection | null;
        selectedObjectIds: string[];
        loading: boolean;
        busy: boolean;
        error: string;
    } | null>(null);
    let waveDataCleanupGeneration = 0;
    let packageOperationGeneration = 0;
    let packageImportAbortController: AbortController | null = null;
    let packageImportRequest = $state<{
        item: DiskTreeItem;
        source: InputFileLocation | null;
        upload: ClientUploadLocation | null;
        sourceName: string;
        inspection: PackageInspection | null;
        plan: ImageSessionPackageImportPlan | null;
        renames: Record<string, string>;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        progress: number;
        error: string;
    } | null>(null);
    let packageExportRequest = $state<{
        items: PackageExportSelection[];
        busy: boolean;
        progressLabel: string;
        error: string;
    } | null>(null);
    type PackageExportDestination =
        | { kind: 'WORKSPACE'; output: { rootId: string; relativePath: string }; overwrite: boolean }
        | { kind: 'DOWNLOAD'; filename: string };
    type CompanionRetry =
        | { kind: 'audition'; objectId: string }
        | { kind: 'sample-bank'; bankId: string }
        | {
              kind: 'package-export';
              destination: PackageExportDestination;
              localDestination?: { candidateId: string };
          };
    let companionDiskRequest = $state<{
        directories: DirectoryRef[];
        retry: CompanionRetry;
        busy: boolean;
        error: string;
    } | null>(null);
    let pendingAuditionObjectId = '';
    let audioFileInput: HTMLInputElement;
    let audioImportRequest = $state<{
        files: (ClientUploadSource | FileLocation)[];
        target: AudioImportTarget;
    } | null>(null);
    let audioDragActive = $state(false);
    let audioDragTarget = $state<AudioImportTarget | null>(null);
    let activeVolumeId = $state('');
    let volumeLoadGeneration = 0;
    let packageExportSelection = $state<PackageExportSelectionState>(emptyPackageExportSelection());
    let auditionState = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    let autoplay = $state(false);
    let playingSampleBankId = $state('');
    let sampleBankPreviewMemberId = $state('');
    let sampleBankPlaybackGeneration = 0;
    let samplePreviewStates = $state<Record<string, Pick<SampleWaveformPreview, 'preview' | 'previewState'>>>({});
    type PreviewTarget = { kind: 'wave-data'; objectId: string; itemId: string } | { kind: 'sample'; objectId: string };
    const previewQueue: { target: PreviewTarget; generation: number }[] = [];
    const previewPending = new Set<string>();
    const previewFailed = new Set<string>();
    let previewInflight = 0;
    let previewGeneration = 0;
    let interfaceScaleState = $state<InterfaceScaleState | null>(null);
    let stopInterfaceScaleSubscription: (() => void) | undefined;
    const auditionController = new AuditionController(transport, (state) => {
        auditionState = state;
        if (
            state.status === 'playing' &&
            state.objectId &&
            playingSampleBankId &&
            membersForBank(playingSampleBankId).some((member) => member.objectId === state.objectId)
        ) {
            sampleBankPreviewMemberId = state.objectId;
        }
        if (state.status === 'failed' && state.error) {
            sourceStatus = state.error;
            if (
                state.errorCode === 'companion_disks_required' &&
                state.objectId &&
                state.objectId === pendingAuditionObjectId
            ) {
                requestCompanionDisks({ kind: 'audition', objectId: state.objectId });
            }
            pendingAuditionObjectId = '';
        } else if (state.status === 'playing') {
            pendingAuditionObjectId = '';
        }
    });

    onDestroy(() => {
        stopInterfaceScaleSubscription?.();
        void interfaceScaling?.dispose();
        ++imageOpenGeneration;
        ++packageOperationGeneration;
        packageImportAbortController?.abort();
        packageImportAbortController = null;
        const packageRequest = packageImportRequest;
        packageImportRequest = null;
        if (packageRequest) void releasePackageImportResources(packageRequest);
        const sessionId = openSessionId;
        openSessionId = null;
        void auditionController.dispose().catch(() => undefined);
        if (sessionId !== null) void transport.closeImage(sessionId).catch(() => undefined);
    });

    function setInterfaceScale(mode: InterfaceScaleMode): void {
        void interfaceScaling?.setMode(mode);
    }

    onMount(() => {
        interfaceScaleState = interfaceScaling?.state() ?? null;
        stopInterfaceScaleSubscription = interfaceScaling?.subscribe((state) => {
            interfaceScaleState = state;
        });
        if (!isDesktop) return;
        let disposed = false;
        let unlisten: (() => void) | null = null;
        void listenForNativeAudioDrops({
            onHover: (active, position) => {
                audioDragActive = active;
                audioDragTarget = active && position ? nativeDroppedAudioTarget(position) : null;
            },
            onDrop: (files, position, droppedPathCount) => {
                reportDiagnostic('native_audio_drop_received', {
                    droppedPathCount,
                    admittedFileCount: files.length,
                });
                if (droppedPathCount > 0 && files.length === 0) {
                    sourceStatus = 'No supported audio files were dropped';
                    return;
                }
                void requestAudioImport(files, nativeDroppedAudioTarget(position));
            },
            onError: (reason) => {
                sourceStatus = 'Dropped audio files could not be read';
                reportError('Read dropped audio files failed', reason);
            },
        })
            .then((stop) => {
                if (disposed) stop();
                else unlisten = stop;
            })
            .catch((reason) => reportError('Initialize native audio drop failed', reason));
        return () => {
            disposed = true;
            unlisten?.();
        };
    });

    function activeAudioTarget(): AudioImportTarget | null {
        return volumeMutationsAvailable &&
            selectedSource.kind === 'volume' &&
            selectedSource.partitionIndex !== undefined
            ? { partitionIndex: selectedSource.partitionIndex, volumeName: selectedSource.name }
            : null;
    }

    function audioSourceName(source: ClientUploadSource | FileLocation): string {
        return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
    }

    async function requestAudioImport(
        files: (ClientUploadSource | FileLocation)[],
        target = activeAudioTarget(),
    ): Promise<void> {
        const admitted: (ClientUploadSource | FileLocation)[] = [];
        for (const source of files) {
            const mediaType = audioMediaType(audioSourceName(source));
            if (!mediaType) continue;
            admitted.push('kind' in source ? source : { ...source, type: mediaType });
        }
        if (files.length > 0 && admitted.length === 0) {
            sourceStatus = 'No supported WAV, FLAC, or AIFF files were selected';
            return;
        }
        if (!target || admitted.length === 0 || !imageLocation) {
            sourceStatus = target ? 'Drop WAV, FLAC, or AIFF audio files' : 'Select a writable volume first';
            return;
        }
        const active = activeAudioTarget();
        if (active?.partitionIndex !== target.partitionIndex || active.volumeName !== target.volumeName) {
            const item = findSourceItem(sourceItems, target.partitionIndex, target.volumeName);
            if (!item || item.kind !== 'volume') {
                sourceStatus = 'Audio import target is no longer available';
                return;
            }
            selectedSource = item;
            await loadVolume(item.id);
            if (activeVolumeId !== item.id) return;
        }
        audioImportRequest = { files: admitted, target };
    }

    function chooseAudioFiles(): void {
        const target = activeAudioTarget();
        if (!target || !imageLocation) {
            sourceStatus = 'Select a writable volume first';
            return;
        }
        audioImportRequest = { files: [], target };
    }

    function filesChosen(event: Event): void {
        const input = event.currentTarget as HTMLInputElement;
        void requestAudioImport(
            Array.from(input.files ?? []).map(browserUploadSource),
            audioImportRequest?.target ?? activeAudioTarget(),
        );
        input.value = '';
    }

    async function chooseWorkspaceAudio(): Promise<void> {
        const request = audioImportRequest;
        if (!request) return;
        const selections = await chooseServerFiles('Choose audio files', [...audioExtensions], {
            parentDialog: 'audio-import',
            initialDirectory: lastAudioDirectory,
            ondirectorychange: (directory) => (lastAudioDirectory = directory),
        });
        if (!selections || !audioImportRequest) return;
        await requestAudioImport(selections, request.target);
    }

    function chooseLocalAudio(): void {
        if (!audioImportRequest || !transport.supportsClientUploads) return;
        audioFileInput.click();
    }

    function audioTargetForElement(target: EventTarget | null): AudioImportTarget | null {
        const element = target instanceof Element ? target.closest<HTMLElement>('[data-audio-drop-volume]') : null;
        const partition = element?.dataset.audioDropPartition;
        if (element?.dataset.audioDropVolume && partition !== undefined) {
            return { partitionIndex: Number(partition), volumeName: element.dataset.audioDropVolume };
        }
        return activeAudioTarget();
    }

    function droppedAudioTarget(event: DragEvent): AudioImportTarget | null {
        return audioTargetForElement(event.target);
    }

    function nativeDroppedAudioTarget(position: NativeDropPosition): AudioImportTarget | null {
        const scale = window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
        const element = document.elementFromPoint?.(position.x / scale, position.y / scale) ?? null;
        return audioTargetForElement(element);
    }

    function dragMayContainFiles(dataTransfer: DataTransfer): boolean {
        if (dataTransfer.files.length > 0) return true;
        if (Array.from(dataTransfer.items ?? []).some((item) => item.kind === 'file')) return true;
        return Array.from(dataTransfer.types).some((type) =>
            ['Files', 'text/uri-list', 'application/x-moz-file'].includes(type),
        );
    }

    function dragAudio(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        if (!dragMayContainFiles(dataTransfer)) {
            dataTransfer.dropEffect = 'none';
            return;
        }
        audioDragTarget = droppedAudioTarget(event);
        dataTransfer.dropEffect = audioDragTarget ? 'copy' : 'none';
        audioDragActive = true;
    }

    function leaveAudio(event: DragEvent): void {
        if (event.relatedTarget !== null) return;
        audioDragActive = false;
        audioDragTarget = null;
    }

    function dropAudio(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        audioDragActive = false;
        const target = droppedAudioTarget(event);
        audioDragTarget = null;
        const files = Array.from(dataTransfer.files).map(browserUploadSource);
        if (files.length === 0) return;
        void requestAudioImport(files, target);
    }

    async function commitAudioImport(items: AudioImportItem[]): Promise<void> {
        if (!audioImportRequest || openSessionId === null)
            throw new Error('Audio import target is no longer available');
        const target = audioImportRequest.target;
        const firstName = items[0]?.sampleName;
        const sessionId = openSessionId;
        const started = performance.now();
        sourceStatus = 'Importing audio';
        try {
            await auditionController.invalidateSession(sessionId);
            const job = await transport.startAudioImport(sessionId, target, items);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Audio import did not complete');
            selectWorkspaceView('samples');
            await refreshOpenImageSession(target);
            const inserted = samples.find((sample) => sample.name === firstName);
            if (inserted) selectSample(inserted);
            reportMutationTiming('audio-import', started, items.length);
        } catch (error) {
            sourceStatus = userFacingMessage(error);
            throw error;
        }
    }

    const selectedProgram = $derived(programs.find((item) => item.objectId === selectedProgramId));
    const selectedBank = $derived(sampleBanks.find((item) => item.objectId === selectedBankId));
    const selectedSample = $derived(samples.find((item) => item.objectId === selectedSampleId));
    const auditionableSampleObjectIds = $derived(auditionableSampleIds(relationships, waveData));
    const auditionableObjectIds = $derived(
        new Set([...auditionableSampleObjectIds, ...waveData.map((item) => item.objectKey)]),
    );
    const auditionableSampleBankObjectIds = $derived(
        new Set(
            sampleBanks
                .filter((bank) =>
                    membersForBank(bank.objectId).some((member) => auditionableSampleObjectIds.has(member.objectId)),
                )
                .map((bank) => bank.objectId),
        ),
    );
    const bankMembers = $derived(selectedBank ? membersForBank(selectedBank.objectId) : []);
    const bankMemberWaveData = $derived(selectedBankMemberId ? waveDataForSample(selectedBankMemberId) : []);
    const sampleWaveData = $derived(selectedSample ? waveDataForSample(selectedSample.objectId) : []);
    const lowerPanelAvailable = $derived(workspaceView !== 'wave-data');
    const auditionAvailable = $derived(workspaceView !== 'programs');
    const auditionActive = $derived(auditionState.status === 'preparing' || auditionState.status === 'playing');
    const auditionBarVisible = $derived(auditionAvailable);
    const auditionLabel = $derived.by(() => {
        if (!auditionState.objectId) return '';
        const sample = samples.find((item) => item.objectId === auditionState.objectId);
        if (playingSampleBankId) {
            const bank = sampleBanks.find((item) => item.objectId === playingSampleBankId);
            return [bank?.name, sample?.name].filter(Boolean).join(' / ');
        }
        return sample?.name ?? waveData.find((item) => item.objectKey === auditionState.objectId)?.name ?? '';
    });
    const activeCollectionObjectId = $derived(
        workspaceView === 'programs'
            ? selectedProgramId
            : workspaceView === 'sample-banks'
              ? selectedBankId
              : workspaceView === 'samples'
                ? selectedSampleId
                : selectedWaveDataId,
    );
    const inspectorSelection = $derived.by<InspectorSelection>(() => selectionForObject(inspectorObjectId));
    const editorSelection = $derived.by<InspectorSelection>(() => selectionForObject(editorObjectIds[workspaceView]));

    function selectionForObject(objectId: string): InspectorSelection {
        const program = programs.find((item) => item.objectId === objectId);
        if (program) return { kind: 'program', program, assignments: assignmentsForProgram(program.objectId) };
        const bank = sampleBanks.find((item) => item.objectId === objectId);
        if (bank) {
            const members = membersForBank(bank.objectId);
            const displayedMemberId = members.some((member) => member.objectId === sampleBankPreviewMemberId)
                ? sampleBankPreviewMemberId
                : (members[0]?.objectId ?? '');
            return {
                kind: 'sample-bank',
                item: bank,
                members,
                memberPreviews: members.map(sampleWaveformPreview),
                displayedMemberId,
            };
        }
        const sample = samples.find((item) => item.objectId === objectId);
        if (sample) {
            return {
                kind: 'sample',
                item: sample,
                memberships: banksForSample(sample.objectId),
                preview: sampleWaveformPreview(sample),
            };
        }
        const waveform = waveData.find((item) => item.objectKey === objectId);
        return waveform ? { kind: 'wave-data', waveData: waveform } : null;
    }

    function sampleWaveformPreview(item: SampleStructureItem): SampleWaveformPreview {
        const stored = samplePreviewStates[item.objectId];
        return {
            item,
            waveData: linkedWaveDataForSample(item.objectId, relationships, waveData),
            preview: stored?.preview ?? null,
            previewState: stored?.previewState ?? 'idle',
        };
    }

    $effect(() => {
        const preview =
            inspectorSelection?.kind === 'sample'
                ? inspectorSelection.preview
                : inspectorSelection?.kind === 'sample-bank'
                  ? inspectorSelection.memberPreviews.find(
                        (member) => member.item.objectId === inspectorSelection.displayedMemberId,
                    )
                  : null;
        if (preview && preview.waveData.length > 0) requestSampleWaveformPreview(preview.item);
    });

    $effect(() => {
        const sessionId = openSessionId;
        const objectId =
            inspectorSelection?.kind === 'sample'
                ? inspectorSelection.item.objectId
                : inspectorSelection?.kind === 'sample-bank'
                  ? inspectorSelection.displayedMemberId
                  : inspectorSelection?.kind === 'wave-data'
                    ? inspectorSelection.waveData.objectKey
                    : null;
        if (sessionId !== null && objectId && auditionableObjectIds.has(objectId)) {
            void auditionController.prefetch(sessionId, objectId);
        }
    });

    function noteName(key: number): string {
        const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
        return `${names[key % 12]}${Math.floor(key / 12) - 2}`;
    }

    function membersForBank(bankId: string): SampleStructureItem[] {
        return orderedSamplesForBank(bankId, relationships, samples);
    }

    function banksForSample(sampleId: string): SampleStructureItem[] {
        const ids = new Set(
            relationships
                .filter((item) => item.targetObjectId === sampleId && item.relationshipType === 'SBAC_SLOT_TO_SBNK')
                .map((item) => item.sourceObjectId),
        );
        return [...ids]
            .map((id) => sampleBanks.find((item) => item.objectId === id))
            .filter((item) => item !== undefined);
    }

    function waveDataForSample(sampleId: string): WaveDataItem[] {
        return distinctWaveDataForSample(sampleId, relationships, waveData);
    }

    function assignmentsForProgram(programId: string): ProgramAssignmentRow[] {
        return relationships
            .filter((item) => item.sourceObjectId === programId && item.relationshipType.startsWith('PROG_ASSIGNMENT_'))
            .toSorted((left, right) => (left.assignmentIndex ?? 0) - (right.assignmentIndex ?? 0))
            .map((relationship) => {
                const target = relationship.targetObjectId ? objectsById.get(relationship.targetObjectId) : undefined;
                const targetName = relationship.targetObjectId
                    ? (sampleBanks.find((item) => item.objectId === relationship.targetObjectId)?.name ??
                      samples.find((item) => item.objectId === relationship.targetObjectId)?.name ??
                      waveData.find((item) => item.objectKey === relationship.targetObjectId)?.name)
                    : undefined;
                return {
                    relationship,
                    targetObjectId: relationship.targetObjectId,
                    targetType: target?.objectType ?? relationship.relationshipType.replace('PROG_ASSIGNMENT_TO_', ''),
                    targetName: targetName || target?.name || relationship.assignmentName || 'Unresolved assignment',
                };
            });
    }

    function setWaveDataObjects(objects: SamplerObject[], names: Map<string, string>): void {
        const previews = new Map(
            waveData.map((item) => [item.id, { waveform: item.waveform, previewState: item.previewState }] as const),
        );
        waveData = objects
            .filter((object) => object.objectType === 'SMPL')
            .map((object) => {
                const preview = previews.get(object.key);
                return {
                    id: object.key,
                    objectKey: object.key,
                    object,
                    name: objectPresentationName(object, names),
                    note: noteName(object.rootKey),
                    duration:
                        object.sampleRate > 0 ? `${(object.frameCount / object.sampleRate).toFixed(2)} s` : 'Unknown',
                    sampleRate: object.sampleRate > 0 ? `${(object.sampleRate / 1000).toFixed(1)} kHz` : 'Unknown',
                    bitDepth: object.sampleWidthBytes > 0 ? `${object.sampleWidthBytes * 8}-bit` : 'Unknown',
                    channels: 'Mono' as const,
                    storedSizeBytes: object.storedSizeBytes,
                    waveform: preview?.waveform ?? [],
                    previewState: preview?.previewState ?? 'idle',
                };
            });
    }

    async function allContentChildren(sessionId: number, parentId: string): Promise<DiskTreeItem[]> {
        return collectPages((offset, limit) => transport.contentChildren(sessionId, parentId, offset, limit), {
            key: (item) => item.id,
            cancelled: () => openSessionId !== sessionId,
        });
    }

    async function visibleObjectNames(sessionId: number, volumeId: string): Promise<Map<string, string>> {
        const volumeChildren = await allContentChildren(sessionId, volumeId);
        const categoryChildren = (
            await Promise.all(
                volumeChildren
                    .filter((item) => item.kind === 'category' && item.childCount > 0)
                    .map((item) => allContentChildren(sessionId, item.id)),
            )
        ).flat();
        return new Map(
            [...volumeChildren, ...categoryChildren]
                .filter((item) => item.objectId)
                .map((item) => [item.objectId!, item.name]),
        );
    }

    async function allObjects(sessionId: number, volumeId: string): Promise<SamplerObject[]> {
        return collectPages(
            async (offset, limit) => {
                const page = await transport.objectPage(sessionId, offset, limit, { scopeId: volumeId });
                return { items: page.objects, totalCount: page.totalCount };
            },
            { key: (item) => item.key, cancelled: () => openSessionId !== sessionId },
        );
    }

    async function allRelationships(sessionId: number, volumeId: string): Promise<SamplerRelationship[]> {
        return collectPages(
            async (offset, limit) => {
                const page = await transport.relationshipPage(sessionId, offset, limit, { scopeId: volumeId });
                return { items: page.relationships, totalCount: page.totalCount };
            },
            {
                key: (item) => item.id,
                cancelled: () => openSessionId !== sessionId,
            },
        );
    }

    async function loadVolume(volumeId: string): Promise<void> {
        if (openSessionId === null) return;
        if (waveDataCleanupRequest?.volumeId !== volumeId) {
            ++waveDataCleanupGeneration;
            waveDataCleanupRequest = null;
        }
        void stopPlaybackNow();
        resetPreviewQueue();
        activeVolumeId = volumeId;
        const sessionId = openSessionId;
        const generation = ++volumeLoadGeneration;
        sourceStatus = 'Loading volume';
        inspectorObjectId = '';
        try {
            const [objects, scopedRelationships, names] = await Promise.all([
                allObjects(sessionId, volumeId),
                allRelationships(sessionId, volumeId),
                visibleObjectNames(sessionId, volumeId),
            ]);
            if (generation !== volumeLoadGeneration) return;
            relationships = scopedRelationships;
            objectsById = new Map(objects.map((object) => [object.key, object]));
            programs = objects
                .filter((object) => object.objectType === 'PROG')
                .map((object) => {
                    const name = objectPresentationName(object, names);
                    const match = /^(\d{3})(?::\s*)?(.*)$/.exec(name);
                    return {
                        id: object.key,
                        objectId: object.key,
                        object,
                        slot: match?.[1] ?? object.name,
                        name: match?.[2] || name,
                    };
                });
            const bankObjects = objects.filter((object) => object.objectType === 'SBAC');
            sampleBanks = bankObjects.map((object) => ({
                id: object.key,
                objectId: object.key,
                object,
                objectType: 'SBAC',
                name: objectPresentationName(object, names),
                memberCount: scopedRelationships.filter(
                    (item) => item.sourceObjectId === object.key && item.relationshipType === 'SBAC_SLOT_TO_SBNK',
                ).length,
            }));
            const sampleObjects = objects.filter((object) => object.objectType === 'SBNK');
            samples = sampleObjects.map((object) => {
                const bankIds = scopedRelationships
                    .filter(
                        (item) => item.targetObjectId === object.key && item.relationshipType === 'SBAC_SLOT_TO_SBNK',
                    )
                    .map((item) => item.sourceObjectId);
                const bankNames = bankIds
                    .map((id) => {
                        const bank = bankObjects.find((candidate) => candidate.key === id);
                        return bank ? objectPresentationName(bank, names) : undefined;
                    })
                    .filter((name): name is string => Boolean(name));
                return {
                    id: object.key,
                    objectId: object.key,
                    object,
                    objectType: 'SBNK',
                    name: objectPresentationName(object, names),
                    membershipLabel:
                        bankNames.length === 0
                            ? 'Standalone'
                            : bankNames.length === 1
                              ? `Sample Bank: ${bankNames[0]}`
                              : `${bankNames.length} Sample Banks`,
                };
            });
            setWaveDataObjects(objects, names);
            sourceObjectCount = objects.length;
            selectedProgramId = '';
            selectedBankId = '';
            sampleBankPreviewMemberId = '';
            selectedBankMemberId = '';
            selectedSampleId = '';
            selectedBankWaveDataId = '';
            selectedSampleWaveDataId = '';
            selectedWaveDataId = '';
            editorObjectIds = { programs: '', 'sample-banks': '', samples: '', 'wave-data': '' };
            sourceStatus = 'Ready';
        } catch (error) {
            if (generation === volumeLoadGeneration) {
                activeVolumeId = '';
                sourceStatus = userFacingMessage(error);
            }
        }
    }

    function clearVolume(): void {
        ++waveDataCleanupGeneration;
        waveDataCleanupRequest = null;
        void stopPlaybackNow();
        resetPreviewQueue();
        ++volumeLoadGeneration;
        programs = [];
        sampleBanks = [];
        samples = [];
        waveData = [];
        relationships = [];
        objectsById = new Map();
        inspectorObjectId = '';
        selectedProgramId = '';
        selectedBankId = '';
        sampleBankPreviewMemberId = '';
        selectedBankMemberId = '';
        selectedSampleId = '';
        selectedBankWaveDataId = '';
        selectedSampleWaveDataId = '';
        selectedWaveDataId = '';
        editorObjectIds = { programs: '', 'sample-banks': '', samples: '', 'wave-data': '' };
        sourceObjectCount = 0;
        activeVolumeId = '';
    }

    function selectSource(item: DiskTreeItem): void {
        selectedSource = item;
        if (item.kind !== 'volume') {
            clearVolume();
            return;
        }
        if (item.id !== activeVolumeId) void loadVolume(item.id);
    }

    function findSourceItem(items: DiskTreeItem[], partitionIndex: number, volumeName?: string): DiskTreeItem | null {
        for (const item of items) {
            if (
                item.partitionIndex === partitionIndex &&
                (volumeName === undefined
                    ? item.kind === 'partition'
                    : item.kind === 'volume' && item.name === volumeName)
            ) {
                return item;
            }
            const nested = findSourceItem(item.children ?? [], partitionIndex, volumeName);
            if (nested) return nested;
        }
        return null;
    }

    function requestImageAction(item: DiskTreeItem, action: ImageTreeAction): void {
        if (item.partitionIndex === undefined) return;
        if (action === 'import-package') {
            if (!packageImportAvailable || item.kind !== 'volume') return;
            ++packageOperationGeneration;
            packageImportAbortController?.abort();
            packageImportAbortController = null;
            selectedSource = item;
            packageImportRequest = {
                item,
                source: null,
                upload: null,
                sourceName: '',
                inspection: null,
                plan: null,
                renames: {},
                status: 'choosing',
                progress: 0,
                error: '',
            };
            return;
        }
        if (action === 'export-package') {
            if (!packageExportAvailable || item.kind !== 'volume') return;
            ++packageOperationGeneration;
            selectedSource = item;
            packageExportRequest = {
                items: [
                    {
                        kind: 'VOLUME',
                        partitionIndex: item.partitionIndex!,
                        volumeName: item.name,
                        name: item.name,
                        typeLabel: 'Volume',
                    },
                ],
                busy: false,
                progressLabel: '',
                error: '',
            };
            return;
        }
        const partitionAction = action === 'rename-partition';
        if (partitionAction && (!partitionMutationsAvailable || item.kind !== 'partition')) return;
        if (!partitionAction && !volumeMutationsAvailable) return;
        if (action === 'add-volume' && item.kind !== 'partition') return;
        if ((action === 'rename-volume' || action === 'delete-volume') && item.kind !== 'volume') return;
        selectedSource = item;
        volumeActionError = '';
        volumeAction = { item, action };
    }

    async function releasePackageImportResources(request: NonNullable<typeof packageImportRequest>): Promise<void> {
        if (request.plan) {
            await transport.releaseImagePackageImportPlan(request.plan.planToken).catch(() => undefined);
        }
        if (request.upload) {
            await transport.releaseClientUpload(request.upload).catch(() => undefined);
        }
    }

    async function closePackageImport(): Promise<void> {
        if (!packageImportRequest || packageImportRequest.status === 'applying') return;
        const request = packageImportRequest;
        packageImportRequest = null;
        ++packageOperationGeneration;
        packageImportAbortController?.abort();
        packageImportAbortController = null;
        await releasePackageImportResources(request);
    }

    async function resetPackageImportSource(): Promise<void> {
        if (!packageImportRequest || packageImportRequest.status === 'applying') return;
        const request = packageImportRequest;
        ++packageOperationGeneration;
        packageImportAbortController?.abort();
        packageImportAbortController = null;
        await releasePackageImportResources(request);
        packageImportRequest = {
            ...request,
            source: null,
            upload: null,
            sourceName: '',
            inspection: null,
            plan: null,
            renames: {},
            status: 'choosing',
            progress: 0,
            error: '',
        };
    }

    async function planSelectedPackage(generation: number, replacePlanToken?: string): Promise<void> {
        const request = packageImportRequest;
        if (
            !request?.source ||
            openSessionId === null ||
            request.item.partitionIndex === undefined ||
            generation !== packageOperationGeneration
        ) {
            return;
        }
        packageImportRequest = {
            ...request,
            status: 'planning',
            plan: replacePlanToken ? request.plan : null,
            error: '',
        };
        const renames = Object.entries(request.renames)
            .map(([nodeId, destinationName]) => ({ nodeId, destinationName: destinationName.trim() }))
            .filter((rename) => rename.destinationName.length > 0);
        const plan = replacePlanToken
            ? await transport.planImagePackageImport(
                  openSessionId,
                  request.source,
                  request.item.partitionIndex,
                  request.item.name,
                  renames,
                  replacePlanToken,
              )
            : await transport.planImagePackageImport(
                  openSessionId,
                  request.source,
                  request.item.partitionIndex,
                  request.item.name,
                  renames,
              );
        if (generation !== packageOperationGeneration || !packageImportRequest) {
            await transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
            return;
        }
        const nextRenames = { ...request.renames };
        for (const action of plan.actions) {
            if (plan.conflicts.some((conflict) => conflict.nodeId === action.nodeId) && !nextRenames[action.nodeId]) {
                nextRenames[action.nodeId] = action.destinationName;
            }
        }
        packageImportRequest = {
            ...packageImportRequest,
            plan,
            renames: nextRenames,
            status: 'ready',
            error: '',
        };
    }

    async function inspectSelectedPackage(
        source: InputFileLocation,
        sourceName: string,
        upload: ClientUploadLocation | null = null,
    ): Promise<void> {
        if (!packageImportRequest) return;
        const generation = ++packageOperationGeneration;
        packageImportRequest = {
            ...packageImportRequest,
            source,
            upload,
            sourceName,
            inspection: null,
            plan: null,
            renames: {},
            status: 'loading',
            progress: 0,
            error: '',
        };
        try {
            const inspection = await transport.inspectPackage(source, false);
            if (generation !== packageOperationGeneration || !packageImportRequest) {
                if (upload) await transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            packageImportRequest = { ...packageImportRequest, inspection, status: 'planning' };
            await planSelectedPackage(generation);
        } catch (error) {
            if (generation !== packageOperationGeneration || !packageImportRequest) {
                if (upload) await transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            packageImportRequest = {
                ...packageImportRequest,
                status: 'choosing',
                error: userFacingMessage(error),
            };
        }
    }

    async function chooseWorkspacePackage(): Promise<void> {
        if (!packageImportRequest) return;
        const selection = await chooseServerLocation('file', 'Choose axklib package', packageExtensions, '', {
            parentDialog: 'package-import',
            initialDirectory: lastPackageDirectory,
            ondirectorychange: (directory) => (lastPackageDirectory = directory),
        });
        if (selection?.kind !== 'server-file' || !packageImportRequest) return;
        await inspectSelectedPackage(selection, selection.displayName);
    }

    async function chooseLocalPackage(): Promise<void> {
        if (!packageImportRequest || !isDesktop) return;
        let controller: AbortController | null = null;
        let generation = -1;
        try {
            const path = await selectLocalPackage();
            if (!path || !packageImportRequest) return;
            const file = await nativeFileSource(path, packageExtensionSet, 'application/octet-stream');
            controller = new AbortController();
            packageImportAbortController?.abort();
            packageImportAbortController = controller;
            generation = ++packageOperationGeneration;
            packageImportRequest = {
                ...packageImportRequest,
                sourceName: file.name,
                source: null,
                upload: null,
                inspection: null,
                plan: null,
                renames: {},
                status: 'loading',
                progress: 0,
                error: '',
            };
            const upload = await transport.uploadClientFile(
                file,
                'PACKAGE',
                (sent, total) => {
                    if (generation === packageOperationGeneration && packageImportRequest) {
                        packageImportRequest = {
                            ...packageImportRequest,
                            progress: total === 0 ? 0 : sent / total,
                        };
                    }
                },
                controller.signal,
            );
            if (packageImportAbortController === controller) packageImportAbortController = null;
            if (generation !== packageOperationGeneration || !packageImportRequest) {
                await transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            await inspectSelectedPackage(upload, file.name, upload);
        } catch (error) {
            if (packageImportAbortController === controller) packageImportAbortController = null;
            if (generation >= 0 && (generation !== packageOperationGeneration || !packageImportRequest)) return;
            reportError('Import local package failed', error);
            if (packageImportRequest) {
                packageImportRequest = {
                    ...packageImportRequest,
                    status: 'choosing',
                    error: userFacingMessage(error),
                };
            }
        }
    }

    async function replanPackageImport(): Promise<void> {
        if (!packageImportRequest?.source || packageImportRequest.status === 'applying') return;
        const previousPlan = packageImportRequest.plan;
        packageImportRequest = { ...packageImportRequest, status: 'planning', error: '' };
        const generation = ++packageOperationGeneration;
        try {
            await planSelectedPackage(generation, previousPlan?.planToken);
        } catch (error) {
            if (generation === packageOperationGeneration && packageImportRequest) {
                packageImportRequest = {
                    ...packageImportRequest,
                    status: 'ready',
                    error: userFacingMessage(error),
                };
            }
        }
    }

    async function applyPackageImport(): Promise<void> {
        const request = packageImportRequest;
        if (!request?.plan?.valid || openSessionId === null || request.item.partitionIndex === undefined) return;
        const sessionId = openSessionId;
        const generation = ++packageOperationGeneration;
        packageImportRequest = { ...request, status: 'applying', error: '' };
        sourceStatus = `Importing package into ${request.item.name}`;
        try {
            await auditionController.invalidateSession(sessionId);
            const job = await transport.startImagePackageImport(request.plan.planToken);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Package import did not complete');
            }
            if (request.upload) await transport.releaseClientUpload(request.upload).catch(() => undefined);
            packageImportRequest = null;
            await refreshOpenImageSession({
                partitionIndex: request.item.partitionIndex,
                volumeName: request.item.name,
            });
            sourceStatus = `Imported package into ${request.item.name}`;
        } catch (error) {
            const message = userFacingMessage(error);
            sourceStatus = message;
            if (generation === packageOperationGeneration && packageImportRequest) {
                packageImportRequest = { ...packageImportRequest, status: 'ready', error: message };
            }
        }
    }

    function requestObjectPackageExport(items: PackageExportObject[]): void {
        if (!packageExportAvailable || items.length === 0 || items.length > maximumPackageExportRoots) return;
        ++packageOperationGeneration;
        packageExportRequest = { items: [...items], busy: false, progressLabel: '', error: '' };
    }

    function clearPackageExportSelection(): void {
        packageExportSelection = emptyPackageExportSelection();
    }

    function reportPackageExportSelectionLimit(): void {
        sourceStatus = `Package export supports at most ${maximumPackageExportRoots.toLocaleString()} selected objects`;
    }

    function packageExportRoots(items: PackageExportSelection[]): ImageSessionPackageExportRoot[] {
        return items.map((item) =>
            item.kind === 'VOLUME'
                ? {
                      kind: 'VOLUME',
                      partitionIndex: item.partitionIndex,
                      volumeName: item.volumeName,
                  }
                : { kind: item.kind, objectId: item.objectId },
        );
    }

    async function runPackageExport(
        destination: PackageExportDestination,
        localDestination?: { candidateId: string },
    ): Promise<void> {
        const request = packageExportRequest;
        if (!request || openSessionId === null) return;
        const sessionId = openSessionId;
        const exportLabel = request.items.length === 1 ? request.items[0]!.name : `${request.items.length} objects`;
        packageExportRequest = { ...request, busy: true, progressLabel: 'Building package', error: '' };
        sourceStatus = `Exporting ${exportLabel}`;
        let retained: ImageSessionPackageExportResult['download'] = null;
        try {
            const job = await transport.startImagePackageExport(
                sessionId,
                packageExportRoots(request.items),
                destination,
            );
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (packageExportRequest && update.progress?.label) {
                    packageExportRequest = { ...packageExportRequest, progressLabel: update.progress.label };
                }
            });
            if (completed.status !== 'completed') {
                if (completed.errorCode === 'companion_disks_required') {
                    packageExportRequest = {
                        ...request,
                        busy: false,
                        progressLabel: '',
                        error: '',
                    };
                    requestCompanionDisks({
                        kind: 'package-export',
                        destination,
                        localDestination,
                    });
                    return;
                }
                throw new Error(completed.error ?? 'Package export did not complete');
            }
            const result = completed.result as ImageSessionPackageExportResult;
            retained = result.download;
            if (localDestination) {
                if (!retained) throw new Error('Package export did not provide a retained download');
                await saveRetainedPackage(localDestination.candidateId, retained.contentPath, retained.sizeBytes);
            }
            packageExportRequest = null;
            sourceStatus = `Exported ${exportLabel}`;
        } catch (error) {
            const message = userFacingMessage(error);
            sourceStatus = message;
            if (packageExportRequest) {
                packageExportRequest = { ...packageExportRequest, busy: false, progressLabel: '', error: message };
            }
        } finally {
            if (retained) await transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    async function exportPackageToWorkspace(): Promise<void> {
        const request = packageExportRequest;
        if (!request || request.busy) return;
        const generation = packageOperationGeneration;
        const filename = packageExportFilename(request.items, imageLocation);
        const selection = await chooseServerLocation(
            'save-file',
            'Export axklib package',
            [filename.slice(filename.lastIndexOf('.') + 1)],
            filename,
            {
                parentDialog: 'package-export',
                initialDirectory: lastPackageDirectory,
                ondirectorychange: (directory) => (lastPackageDirectory = directory),
            },
        );
        if (selection?.kind !== 'server-file' || generation !== packageOperationGeneration || !packageExportRequest) {
            return;
        }
        await runPackageExport({ kind: 'WORKSPACE', output: selection.reference, overwrite: false });
    }

    async function exportPackageToComputer(): Promise<void> {
        const request = packageExportRequest;
        if (!request || request.busy || !isDesktop) return;
        const generation = packageOperationGeneration;
        try {
            const destination = await selectLocalPackageDestination(
                packageExportFilename(request.items, imageLocation),
            );
            if (!destination || generation !== packageOperationGeneration || !packageExportRequest) return;
            await runPackageExport(
                { kind: 'DOWNLOAD', filename: destination.filename },
                { candidateId: destination.candidateId },
            );
        } catch (error) {
            if (packageExportRequest) {
                const message = userFacingMessage(error);
                packageExportRequest = { ...packageExportRequest, error: message };
                sourceStatus = message;
            }
        }
    }

    function cancelObjectDeletion(): void {
        if (objectDeletionRequest?.busy) return;
        ++objectDeletionGeneration;
        objectDeletionRequest = null;
    }

    function deletionRequestKey(targets: PackageExportObject[]): string {
        return targets
            .map((target) => target.objectId)
            .toSorted()
            .join('\u0000');
    }

    function deletionInspectionFingerprint(inspection: ObjectDeletionInspection): string {
        return JSON.stringify({
            selected: inspection.selectedObjectIds.toSorted(),
            targets: inspection.impacts
                .filter((impact) => impact.role === 'TARGET')
                .map((impact) => [impact.objectId, impact.status, impact.reason])
                .toSorted(([left], [right]) => left.localeCompare(right)),
        });
    }

    function requestObjectDeletion(targets: PackageExportObject[]): void {
        if (!objectDeletionAvailable || openSessionId === null || targets.length === 0) return;
        const generation = ++objectDeletionGeneration;
        objectDeletionRequest = {
            targets,
            cleanupObjectIds: [],
            inspection: null,
            loading: true,
            busy: false,
            error: '',
        };
        void stopPlaybackNow();
        void inspectObjectDeletion(generation);
    }

    async function inspectObjectDeletion(generation = objectDeletionGeneration): Promise<void> {
        const request = objectDeletionRequest;
        const sessionId = openSessionId;
        if (!request || sessionId === null) return;
        try {
            const inspection = await transport.inspectObjectDeletion(
                sessionId,
                request.targets.map((target) => target.objectId),
                request.cleanupObjectIds,
            );
            if (
                generation !== objectDeletionGeneration ||
                deletionRequestKey(objectDeletionRequest?.targets ?? []) !== deletionRequestKey(request.targets) ||
                openSessionId !== sessionId
            ) {
                return;
            }
            objectDeletionRequest = {
                ...request,
                inspection,
                loading: false,
                error: '',
            };
        } catch (error) {
            if (
                generation !== objectDeletionGeneration ||
                deletionRequestKey(objectDeletionRequest?.targets ?? []) !== deletionRequestKey(request.targets)
            ) {
                return;
            }
            objectDeletionRequest = {
                ...request,
                inspection: null,
                loading: false,
                error: userFacingMessage(error),
            };
        }
    }

    function updateObjectDeletionSelection(objectId: string, selected: boolean): void {
        const request = objectDeletionRequest;
        const inspection = request?.inspection;
        if (!request || !inspection || request.busy) return;
        const included = new Set(
            inspection.impacts
                .filter((impact) => impact.role === 'DEPENDENCY' && impact.selected)
                .map((impact) => impact.objectId),
        );
        if (selected) {
            if (request.targets.length + included.size >= maximumPackageExportRoots) {
                sourceStatus = `Deletion is limited to ${maximumPackageExportRoots} targets and cleanup objects`;
                return;
            }
            included.add(objectId);
        } else {
            included.delete(objectId);
            let changed = true;
            while (changed) {
                changed = false;
                for (const impact of inspection.impacts) {
                    if (
                        included.has(impact.objectId) &&
                        impact.prerequisiteObjectIds.some((prerequisite) =>
                            inspection.impacts.some(
                                (candidate) =>
                                    candidate.objectId === prerequisite &&
                                    candidate.role === 'DEPENDENCY' &&
                                    !included.has(prerequisite),
                            ),
                        )
                    ) {
                        included.delete(impact.objectId);
                        changed = true;
                    }
                }
            }
        }
        const generation = ++objectDeletionGeneration;
        objectDeletionRequest = {
            ...request,
            cleanupObjectIds: [...included],
            loading: true,
            error: '',
        };
        void inspectObjectDeletion(generation);
    }

    function updateAllObjectDeletionDependencies(selected: boolean): void {
        const request = objectDeletionRequest;
        const inspection = request?.inspection;
        if (!request || !inspection || request.busy) return;
        const cleanupCapacity = Math.max(0, maximumPackageExportRoots - request.targets.length);
        const cleanupObjectIds = selected
            ? inspection.impacts
                  .filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL')
                  .map((impact) => impact.objectId)
                  .slice(0, cleanupCapacity)
            : [];
        if (
            selected &&
            inspection.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL').length >
                cleanupCapacity
        ) {
            sourceStatus = `Deletion is limited to ${maximumPackageExportRoots} targets and cleanup objects`;
        }
        const generation = ++objectDeletionGeneration;
        objectDeletionRequest = {
            ...request,
            cleanupObjectIds,
            loading: true,
            error: '',
        };
        void inspectObjectDeletion(generation);
    }

    async function submitObjectDeletion(): Promise<void> {
        const request = objectDeletionRequest;
        const sessionId = openSessionId;
        if (!request || !request.inspection?.canApply || request.loading || request.busy || sessionId === null) return;
        const generation = objectDeletionGeneration;
        const preferred =
            selectedSource.kind === 'volume' && selectedSource.partitionIndex !== undefined
                ? { partitionIndex: selectedSource.partitionIndex, volumeName: selectedSource.name }
                : undefined;
        const started = performance.now();
        objectDeletionRequest = { ...request, busy: true, error: '' };
        sourceStatus = `Deleting ${request.inspection.selectedObjectIds.length} ${
            request.inspection.selectedObjectIds.length === 1 ? 'object' : 'objects'
        }`;
        try {
            await auditionController.invalidateSession(sessionId);
            const finalInspection = await transport.inspectObjectDeletion(
                sessionId,
                request.targets.map((target) => target.objectId),
                request.cleanupObjectIds,
            );
            if (!finalInspection.canApply) {
                objectDeletionRequest = {
                    ...request,
                    inspection: finalInspection,
                    busy: false,
                };
                sourceStatus = 'Deletion is blocked; review the affected references';
                return;
            }
            if (deletionInspectionFingerprint(finalInspection) !== deletionInspectionFingerprint(request.inspection)) {
                objectDeletionRequest = {
                    ...request,
                    inspection: finalInspection,
                    cleanupObjectIds: finalInspection.impacts
                        .filter((impact) => impact.role === 'DEPENDENCY' && impact.selected)
                        .map((impact) => impact.objectId),
                    busy: false,
                    error: 'The deletion impact changed. Review the affected objects before confirming again.',
                };
                sourceStatus = 'Deletion impact changed; review before confirming again';
                return;
            }
            const job = await transport.startObjectDeletion(
                sessionId,
                request.targets.map((target) => target.objectId),
                request.cleanupObjectIds,
            );
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Object deletion did not complete');
            }
            await refreshOpenImageSession(preferred);
            const deletedIds = new Set(finalInspection.selectedObjectIds);
            packageExportSelection = {
                ...packageExportSelection,
                items: packageExportSelection.items.filter((item) => !deletedIds.has(item.objectId)),
            };
            ++objectDeletionGeneration;
            objectDeletionRequest = null;
            reportMutationTiming('delete-object', started, finalInspection.selectedObjectIds.length);
        } catch (error) {
            const message = userFacingMessage(error);
            sourceStatus = message;
            if (openSessionId === sessionId) {
                await refreshOpenImageSession(preferred).catch(() => undefined);
            }
            if (
                generation === objectDeletionGeneration &&
                deletionRequestKey(objectDeletionRequest?.targets ?? []) === deletionRequestKey(request.targets) &&
                openSessionId === sessionId
            ) {
                objectDeletionRequest = {
                    ...request,
                    busy: false,
                    loading: true,
                    error: `${message} The image has been refreshed; review the deletion again.`,
                };
                const nextGeneration = ++objectDeletionGeneration;
                await inspectObjectDeletion(nextGeneration);
                if (
                    nextGeneration === objectDeletionGeneration &&
                    deletionRequestKey(objectDeletionRequest?.targets ?? []) === deletionRequestKey(request.targets)
                ) {
                    objectDeletionRequest = {
                        ...request,
                        error: `${message} The image has been refreshed; review the deletion again.`,
                    };
                }
            }
        }
    }

    function cancelWaveDataCleanup(): void {
        if (waveDataCleanupRequest?.busy) return;
        ++waveDataCleanupGeneration;
        waveDataCleanupRequest = null;
    }

    function waveDataCleanupFingerprint(inspection: WaveDataOrphanInspection): string {
        return JSON.stringify(
            inspection.candidates
                .map((candidate) => [
                    candidate.objectId,
                    candidate.storedSizeBytes,
                    candidate.recoverableBytes,
                    candidate.recoverableClusters,
                ])
                .toSorted(([left], [right]) => String(left).localeCompare(String(right))),
        );
    }

    function requestWaveDataCleanup(): void {
        if (
            !waveDataCleanupAvailable ||
            openSessionId === null ||
            activeVolumeId === '' ||
            selectedSource.kind !== 'volume'
        ) {
            return;
        }
        const generation = ++waveDataCleanupGeneration;
        waveDataCleanupRequest = {
            volumeId: activeVolumeId,
            volumeName: selectedSource.name,
            inspection: null,
            selectedObjectIds: [],
            loading: true,
            busy: false,
            error: '',
        };
        void stopPlaybackNow();
        void inspectWaveDataCleanup(generation);
    }

    async function inspectWaveDataCleanup(generation = waveDataCleanupGeneration): Promise<void> {
        const request = waveDataCleanupRequest;
        const sessionId = openSessionId;
        if (!request || sessionId === null) return;
        try {
            const inspection = await transport.inspectWaveDataOrphans(sessionId, request.volumeId);
            if (
                generation !== waveDataCleanupGeneration ||
                openSessionId !== sessionId ||
                activeVolumeId !== request.volumeId ||
                waveDataCleanupRequest?.volumeId !== request.volumeId
            ) {
                return;
            }
            waveDataCleanupRequest = {
                ...request,
                inspection,
                selectedObjectIds: inspection.candidates.map((candidate) => candidate.objectId),
                loading: false,
                error: '',
            };
        } catch (error) {
            if (
                generation !== waveDataCleanupGeneration ||
                openSessionId !== sessionId ||
                waveDataCleanupRequest?.volumeId !== request.volumeId
            ) {
                return;
            }
            waveDataCleanupRequest = {
                ...request,
                inspection: null,
                selectedObjectIds: [],
                loading: false,
                error: userFacingMessage(error),
            };
        }
    }

    function updateWaveDataCleanupSelection(objectId: string, selected: boolean): void {
        const request = waveDataCleanupRequest;
        if (!request || request.loading || request.busy) return;
        const selectedIds = new Set(request.selectedObjectIds);
        if (selected) selectedIds.add(objectId);
        else selectedIds.delete(objectId);
        waveDataCleanupRequest = { ...request, selectedObjectIds: [...selectedIds] };
    }

    function updateAllWaveDataCleanupSelection(selected: boolean): void {
        const request = waveDataCleanupRequest;
        if (!request?.inspection || request.loading || request.busy) return;
        waveDataCleanupRequest = {
            ...request,
            selectedObjectIds: selected ? request.inspection.candidates.map((candidate) => candidate.objectId) : [],
        };
    }

    async function submitWaveDataCleanup(): Promise<void> {
        const request = waveDataCleanupRequest;
        const sessionId = openSessionId;
        if (
            !request?.inspection ||
            request.selectedObjectIds.length === 0 ||
            request.loading ||
            request.busy ||
            sessionId === null
        ) {
            return;
        }
        const selectedIds = request.selectedObjectIds.toSorted();
        const preferred =
            selectedSource.kind === 'volume' && selectedSource.partitionIndex !== undefined
                ? { partitionIndex: selectedSource.partitionIndex, volumeName: selectedSource.name }
                : undefined;
        const started = performance.now();
        waveDataCleanupRequest = { ...request, busy: true, error: '' };
        sourceStatus = `Deleting ${selectedIds.length} unreferenced Wave Data ${
            selectedIds.length === 1 ? 'object' : 'objects'
        }`;
        try {
            await auditionController.invalidateSession(sessionId);
            const rediscovered = await transport.inspectWaveDataOrphans(sessionId, request.volumeId);
            if (
                waveDataCleanupFingerprint(rediscovered) !== waveDataCleanupFingerprint(request.inspection) ||
                selectedIds.some(
                    (objectId) => !rediscovered.candidates.some((candidate) => candidate.objectId === objectId),
                )
            ) {
                waveDataCleanupRequest = {
                    ...request,
                    inspection: rediscovered,
                    selectedObjectIds: rediscovered.candidates.map((candidate) => candidate.objectId),
                    busy: false,
                    error: 'The cleanup candidates changed. Review the current list before confirming again.',
                };
                sourceStatus = 'Cleanup candidates changed; review before confirming again';
                return;
            }
            const deletion = await transport.inspectObjectDeletion(sessionId, selectedIds, []);
            const targetImpacts = deletion.impacts.filter((impact) => impact.role === 'TARGET');
            const eligibleIds = deletion.selectedObjectIds.toSorted();
            if (
                !deletion.canApply ||
                targetImpacts.length !== selectedIds.length ||
                targetImpacts.some((impact) => impact.status !== 'REQUIRED' || !impact.selected) ||
                eligibleIds.join('\u0000') !== selectedIds.join('\u0000')
            ) {
                const current = await transport.inspectWaveDataOrphans(sessionId, request.volumeId);
                waveDataCleanupRequest = {
                    ...request,
                    inspection: current,
                    selectedObjectIds: current.candidates.map((candidate) => candidate.objectId),
                    busy: false,
                    error: 'One or more Wave Data objects are no longer safe to delete. Review the current list.',
                };
                sourceStatus = 'Wave Data cleanup requires another review';
                return;
            }
            const job = await transport.startObjectDeletion(sessionId, selectedIds, []);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed')
                throw new Error(completed.error ?? 'Wave Data cleanup did not complete');
            await refreshOpenImageSession(preferred);
            const deletedIds = new Set(selectedIds);
            packageExportSelection = {
                ...packageExportSelection,
                items: packageExportSelection.items.filter((item) => !deletedIds.has(item.objectId)),
            };
            ++waveDataCleanupGeneration;
            waveDataCleanupRequest = null;
            reportMutationTiming('cleanup-wave-data', started, selectedIds.length);
        } catch (error) {
            const message = userFacingMessage(error);
            sourceStatus = message;
            ++waveDataCleanupGeneration;
            waveDataCleanupRequest = null;
            if (openSessionId === sessionId) await refreshOpenImageSession(preferred).catch(() => undefined);
            sourceStatus = `${message} The image has been refreshed.`;
        }
    }

    async function submitVolumeAction(name: string): Promise<void> {
        if (!volumeAction || !imageLocation || volumeAction.item.partitionIndex === undefined) return;
        const requested = volumeAction;
        const partitionIndex = requested.item.partitionIndex;
        if (partitionIndex === undefined) return;
        const previousVolumeName = requested.item.kind === 'volume' ? requested.item.name : undefined;
        const volumeMutation: VolumeMutation | null =
            requested.action === 'add-volume'
                ? { kind: 'add', partitionIndex, volumeName: name }
                : requested.action === 'rename-volume'
                  ? {
                        kind: 'rename',
                        partitionIndex,
                        volumeName: requested.item.name,
                        newVolumeName: name,
                    }
                  : requested.action === 'delete-volume'
                    ? { kind: 'delete', partitionIndex, volumeName: requested.item.name }
                    : null;
        const partitionMutation: PartitionMutation | null =
            requested.action === 'rename-partition'
                ? {
                      kind: 'rename',
                      partitionIndex,
                      partitionName: requested.item.name,
                      newPartitionName: name,
                  }
                : null;
        const preferredVolumeName =
            requested.action === 'add-volume' || requested.action === 'rename-volume' ? name : undefined;

        volumeActionBusy = true;
        volumeActionError = '';
        sourceStatus =
            requested.action === 'add-volume'
                ? 'Adding volume'
                : requested.action === 'delete-volume'
                  ? 'Deleting volume'
                  : requested.action === 'rename-partition'
                    ? 'Renaming partition'
                    : 'Renaming volume';
        if (openSessionId === null) {
            volumeActionError = 'Image session is no longer available';
            volumeActionBusy = false;
            return;
        }
        const sessionId = openSessionId;
        const started = performance.now();
        try {
            await auditionController.invalidateSession(sessionId);
            const job = partitionMutation
                ? await transport.startPartitionMutation(sessionId, partitionMutation)
                : await transport.startVolumeMutation(sessionId, volumeMutation!);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Image change did not complete');
            }
            volumeAction = null;
            await refreshOpenImageSession({ partitionIndex, volumeName: preferredVolumeName });
            reportMutationTiming(requested.action, started, 1);
        } catch (error) {
            volumeActionError = userFacingMessage(error);
            sourceStatus = volumeActionError;
            if (openSessionId !== null)
                await refreshOpenImageSession({ partitionIndex, volumeName: previousVolumeName }).catch(
                    () => undefined,
                );
        } finally {
            volumeActionBusy = false;
        }
    }

    interface ObjectSelectionSnapshot {
        view: WorkspaceView;
        programId: string;
        bankId: string;
        bankMemberId: string;
        sampleId: string;
        bankWaveDataId: string;
        sampleWaveDataId: string;
        waveDataId: string;
        inspectorId: string;
        editorIds: Record<WorkspaceView, string>;
    }

    function captureObjectSelection(): ObjectSelectionSnapshot {
        return {
            view: workspaceView,
            programId: selectedProgramId,
            bankId: selectedBankId,
            bankMemberId: selectedBankMemberId,
            sampleId: selectedSampleId,
            bankWaveDataId: selectedBankWaveDataId,
            sampleWaveDataId: selectedSampleWaveDataId,
            waveDataId: selectedWaveDataId,
            inspectorId: inspectorObjectId,
            editorIds: { ...editorObjectIds },
        };
    }

    function restoreObjectSelection(snapshot: ObjectSelectionSnapshot, renamedObjectId: string): void {
        const exists = (objectId: string): boolean => Boolean(objectId && objectsById.has(objectId));
        workspaceView = snapshot.view;
        selectedProgramId = exists(snapshot.programId) ? snapshot.programId : '';
        selectedBankId = exists(snapshot.bankId) ? snapshot.bankId : '';
        selectedBankMemberId = exists(snapshot.bankMemberId) ? snapshot.bankMemberId : '';
        selectedSampleId = exists(snapshot.sampleId) ? snapshot.sampleId : '';
        selectedBankWaveDataId = exists(snapshot.bankWaveDataId) ? snapshot.bankWaveDataId : '';
        selectedSampleWaveDataId = exists(snapshot.sampleWaveDataId) ? snapshot.sampleWaveDataId : '';
        selectedWaveDataId = exists(snapshot.waveDataId) ? snapshot.waveDataId : '';
        inspectorObjectId = exists(renamedObjectId)
            ? renamedObjectId
            : exists(snapshot.inspectorId)
              ? snapshot.inspectorId
              : '';
        editorObjectIds = {
            programs: exists(snapshot.editorIds.programs) ? snapshot.editorIds.programs : '',
            'sample-banks': exists(snapshot.editorIds['sample-banks']) ? snapshot.editorIds['sample-banks'] : '',
            samples: exists(snapshot.editorIds.samples) ? snapshot.editorIds.samples : '',
            'wave-data': exists(snapshot.editorIds['wave-data']) ? snapshot.editorIds['wave-data'] : '',
        };
    }

    function requestObjectRename(target: ObjectRenameTarget): void {
        if (!objectRenameAvailable || openSessionId === null) return;
        objectRenameRequest = { target, busy: false, error: '' };
    }

    function objectRenameMutation(target: ObjectRenameTarget, name: string): ObjectRenameMutation {
        const common = {
            partitionIndex: target.object.partitionIndex,
            volumeName: target.object.volumeName,
        };
        if (target.kind === 'program') {
            return {
                ...common,
                kind: 'program',
                programNumber: target.programNumber,
                newProgramName: name,
            };
        }
        if (target.kind === 'sample-bank') {
            return {
                ...common,
                kind: 'sample-bank',
                sampleBankName: target.name,
                newSampleBankName: name,
            };
        }
        if (target.kind === 'sample') {
            return {
                ...common,
                kind: 'sample',
                sampleName: target.name,
                newSampleName: name,
            };
        }
        return {
            ...common,
            kind: 'wave-data',
            waveformName: target.name,
            newWaveformName: name,
        };
    }

    async function submitObjectRename(name: string): Promise<void> {
        const request = objectRenameRequest;
        const sessionId = openSessionId;
        if (!request || request.busy || sessionId === null) return;
        const target = request.target;
        const selection = captureObjectSelection();
        const preferred = {
            partitionIndex: target.object.partitionIndex,
            volumeName: target.object.volumeName,
        };
        const started = performance.now();
        objectRenameRequest = { ...request, busy: true, error: '' };
        sourceStatus = `Renaming ${target.name}`;
        try {
            await auditionController.invalidateSession(sessionId);
            const job = await transport.startObjectRename(sessionId, objectRenameMutation(target, name));
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Object rename did not complete');
            }
            await refreshOpenImageSession(preferred);
            restoreObjectSelection(selection, target.object.key);
            objectRenameRequest = null;
            sourceStatus = `Renamed ${target.name} to ${name}`;
            reportMutationTiming('rename-object', started, 1);
        } catch (error) {
            const message = userFacingMessage(error);
            if (openSessionId === sessionId) {
                await refreshOpenImageSession(preferred).catch(() => undefined);
                restoreObjectSelection(selection, target.object.key);
            }
            if (objectRenameRequest?.target.object.key === target.object.key) {
                objectRenameRequest = { ...objectRenameRequest, busy: false, error: message };
            }
            sourceStatus = message;
        }
    }

    function selectProgram(program: Program): void {
        selectedProgramId = program.objectId;
        setEditorObject(program.objectId);
        void inspectObject(program.objectId);
    }

    async function selectBank(item: SampleStructureItem, playAfterSelection = autoplay): Promise<void> {
        selectedBankId = item.objectId;
        resetSampleBankPreview(item.objectId);
        selectedBankMemberId = '';
        selectedBankWaveDataId = '';
        setEditorObject(item.objectId);
        await inspectObject(item.objectId);
        if (
            playAfterSelection &&
            workspaceView === 'sample-banks' &&
            selectedBankId === item.objectId &&
            auditionableSampleBankObjectIds.has(item.objectId)
        ) {
            playSampleBank(item);
        }
    }

    async function selectSample(item: SampleStructureItem, playAfterSelection = autoplay): Promise<void> {
        selectedSampleId = item.objectId;
        selectedSampleWaveDataId = '';
        setEditorObject(item.objectId);
        await inspectObject(item.objectId);
        if (
            playAfterSelection &&
            workspaceView === 'samples' &&
            selectedSampleId === item.objectId &&
            auditionableSampleObjectIds.has(item.objectId)
        ) {
            playObject(item.objectId);
        }
    }

    async function selectBankMember(item: SampleStructureItem, playAfterSelection = autoplay): Promise<void> {
        selectedBankMemberId = item.objectId;
        selectedBankWaveDataId = '';
        setEditorObject(item.objectId);
        await inspectObject(item.objectId);
        if (
            playAfterSelection &&
            workspaceView === 'sample-banks' &&
            selectedBankMemberId === item.objectId &&
            auditionableSampleObjectIds.has(item.objectId)
        ) {
            playObject(item.objectId);
        }
    }

    function setEditorObject(objectId: string): void {
        editorObjectIds[workspaceView] = objectId;
    }

    function selectAssignment(row: ProgramAssignmentRow): void {
        if (!row.targetObjectId) return;
        void inspectObject(row.targetObjectId);
    }

    function inspectObject(objectId: string): Promise<void> {
        const stopPlayback =
            Boolean(playingSampleBankId) || inspectorSelectionStopsPlayback(auditionState.objectId, objectId);
        inspectorObjectId = objectId;
        inspectorOpen = true;
        return stopPlayback ? stopPlaybackNow() : Promise.resolve();
    }

    async function selectWaveData(item: WaveDataItem, playAfterSelection = autoplay): Promise<void> {
        if (workspaceView === 'sample-banks') selectedBankWaveDataId = item.objectKey;
        else if (workspaceView === 'samples') selectedSampleWaveDataId = item.objectKey;
        else if (workspaceView === 'wave-data') selectedWaveDataId = item.objectKey;
        setEditorObject(item.objectKey);
        requestWaveformPreview(item);
        await inspectObject(item.objectKey);
        const selectionStillActive =
            (workspaceView === 'sample-banks' && selectedBankWaveDataId === item.objectKey) ||
            (workspaceView === 'samples' && selectedSampleWaveDataId === item.objectKey) ||
            (workspaceView === 'wave-data' && selectedWaveDataId === item.objectKey);
        if (playAfterSelection && selectionStillActive) playObject(item.objectKey);
    }

    function resetPreviewQueue(): void {
        previewGeneration += 1;
        previewQueue.length = 0;
        previewPending.clear();
        previewFailed.clear();
        samplePreviewStates = {};
    }

    function requestWaveformPreview(item: WaveDataItem): void {
        if (
            openSessionId === null ||
            item.previewState === 'ready' ||
            previewPending.has(item.objectKey) ||
            previewFailed.has(item.objectKey)
        ) {
            return;
        }
        previewPending.add(item.objectKey);
        waveData = waveData.map((candidate) =>
            candidate.id === item.id ? { ...candidate, previewState: 'loading' } : candidate,
        );
        previewQueue.push({
            target: { kind: 'wave-data', objectId: item.objectKey, itemId: item.id },
            generation: previewGeneration,
        });
        drainPreviewQueue();
    }

    function requestSampleWaveformPreview(item: SampleStructureItem): void {
        const state = samplePreviewStates[item.objectId];
        if (
            openSessionId === null ||
            state?.previewState === 'ready' ||
            previewPending.has(item.objectId) ||
            previewFailed.has(item.objectId)
        ) {
            return;
        }
        previewPending.add(item.objectId);
        samplePreviewStates = {
            ...samplePreviewStates,
            [item.objectId]: { preview: null, previewState: 'loading' },
        };
        previewQueue.push({
            target: { kind: 'sample', objectId: item.objectId },
            generation: previewGeneration,
        });
        drainPreviewQueue();
    }

    function drainPreviewQueue(): void {
        while (previewInflight < 2 && previewQueue.length > 0 && openSessionId !== null) {
            const queued = previewQueue.shift();
            if (!queued) return;
            const { target, generation } = queued;
            const sessionId = openSessionId;
            previewInflight += 1;
            void transport
                .preview(sessionId, target.objectId, 1024)
                .then((preview) => {
                    if (openSessionId !== sessionId || previewGeneration !== generation) return;
                    if (target.kind === 'wave-data') {
                        const lane = preview.lanes[0];
                        if (!lane || lane.sourceObjectId !== target.objectId) {
                            throw new Error('Wave Data preview did not return its physical waveform lane');
                        }
                        waveData = waveData.map((candidate) =>
                            candidate.id === target.itemId
                                ? { ...candidate, waveform: lane.bins, previewState: 'ready' }
                                : candidate,
                        );
                    } else {
                        samplePreviewStates = {
                            ...samplePreviewStates,
                            [target.objectId]: { preview, previewState: 'ready' },
                        };
                    }
                })
                .catch((error) => {
                    if (openSessionId !== sessionId || previewGeneration !== generation) return;
                    previewFailed.add(target.objectId);
                    if (target.kind === 'wave-data') {
                        waveData = waveData.map((candidate) =>
                            candidate.id === target.itemId ? { ...candidate, previewState: 'failed' } : candidate,
                        );
                    } else {
                        samplePreviewStates = {
                            ...samplePreviewStates,
                            [target.objectId]: { preview: null, previewState: 'failed' },
                        };
                    }
                    sourceStatus = userFacingMessage(error);
                })
                .finally(() => {
                    if (previewGeneration === generation) previewPending.delete(target.objectId);
                    previewInflight -= 1;
                    drainPreviewQueue();
                });
        }
    }

    async function playWaveData(item: WaveDataItem): Promise<void> {
        await selectWaveData(item, false);
        playObject(item.objectKey);
    }

    async function playSample(item: SampleStructureItem): Promise<void> {
        if (!auditionableSampleObjectIds.has(item.objectId)) {
            sourceStatus = 'This Sample has no confirmed Wave Data to audition';
            return;
        }
        await (workspaceView === 'sample-banks' ? selectBankMember(item, false) : selectSample(item, false));
        playObject(item.objectId);
    }

    async function playSampleBank(item: SampleStructureItem): Promise<void> {
        if (openSessionId === null) return;
        if (selectedBankId !== item.objectId) await selectBank(item, false);
        if (openSessionId === null) return;
        const members = membersForBank(item.objectId);
        const unavailableMember = members.find((member) => !auditionableSampleObjectIds.has(member.objectId));
        if (unavailableMember) {
            sampleBankPreviewMemberId = unavailableMember.objectId;
            sourceStatus = `Sample Bank audition requires playable Wave Data for ${unavailableMember.name}`;
            return;
        }
        const memberIds = members.map((member) => member.objectId);
        if (memberIds.length === 0) {
            sampleBankPreviewMemberId = '';
            sourceStatus = 'This Sample Bank has no playable Samples';
            return;
        }
        const generation = ++sampleBankPlaybackGeneration;
        playingSampleBankId = item.objectId;
        sampleBankPreviewMemberId = memberIds[0] ?? '';
        auditionController.playSequence(openSessionId, memberIds, (result) => {
            if (generation !== sampleBankPlaybackGeneration) return;
            playingSampleBankId = '';
            resetSampleBankPreview(item.objectId);
            if (result.status === 'failed') {
                sourceStatus = result.error;
                if (result.errorCode === 'companion_disks_required') {
                    requestCompanionDisks({ kind: 'sample-bank', bankId: item.objectId });
                }
            } else if (result.status === 'completed' && result.playedCount === 0) {
                sourceStatus = 'This Sample Bank has no playable Samples';
            }
        });
    }

    function resetSampleBankPreview(bankId = selectedBankId): void {
        sampleBankPreviewMemberId = bankId ? (membersForBank(bankId)[0]?.objectId ?? '') : '';
    }

    function cancelSampleBankPlayback(): void {
        const bankId = playingSampleBankId || selectedBankId;
        sampleBankPlaybackGeneration += 1;
        playingSampleBankId = '';
        resetSampleBankPreview(bankId);
    }

    function stopPlaybackNow(): Promise<void> {
        cancelSampleBankPlayback();
        return auditionController.stop();
    }

    function currentPlaybackRowVisible(): boolean {
        const queries = laneQueries[workspaceView];
        const visibleSampleBankIds =
            workspaceView === 'sample-banks'
                ? sampleBanks.filter((item) => matchesSearch(item.name, queries.primary)).map((item) => item.objectId)
                : [];
        const visibleSampleIds =
            workspaceView === 'sample-banks'
                ? bankMembers.filter((item) => matchesSearch(item.name, queries.secondary)).map((item) => item.objectId)
                : workspaceView === 'samples'
                  ? samples.filter((item) => matchesSearch(item.name, queries.primary)).map((item) => item.objectId)
                  : [];
        const visibleWaveDataIds =
            workspaceView === 'sample-banks'
                ? bankMemberWaveData
                      .filter((item) => matchesSearch(item.name, queries.tertiary))
                      .map((item) => item.objectKey)
                : workspaceView === 'samples'
                  ? sampleWaveData
                        .filter((item) => matchesSearch(item.name, queries.secondary))
                        .map((item) => item.objectKey)
                  : workspaceView === 'wave-data'
                    ? waveData.filter((item) => matchesSearch(item.name, queries.primary)).map((item) => item.objectKey)
                    : [];
        return playbackRowVisible({
            view: workspaceView,
            playingSampleBankId,
            playingObjectId: auditionState.objectId,
            visibleSampleBankIds,
            visibleSampleIds,
            visibleWaveDataIds,
        });
    }

    function updateLaneQuery(view: WorkspaceView, lane: keyof LaneQueries, value: string): void {
        laneQueries[view][lane] = value;
        if (view === workspaceView && auditionActive && !currentPlaybackRowVisible()) void stopPlaybackNow();
    }

    function selectWorkspaceView(view: WorkspaceView): void {
        if (workspaceView === view) return;
        if (auditionActive) void stopPlaybackNow();
        workspaceView = view;
    }

    async function playContainedWaveData(item: WaveDataItem): Promise<void> {
        await selectWaveData(item, false);
        playObject(item.objectKey);
    }

    function playObject(objectId: string): void {
        if (openSessionId === null) return;
        if (!auditionableObjectIds.has(objectId)) {
            sourceStatus = 'This Sample has no confirmed Wave Data to audition';
            return;
        }
        cancelSampleBankPlayback();
        pendingAuditionObjectId = objectId;
        void auditionController.play(openSessionId, objectId);
    }

    function prefetchObject(objectId: string): void {
        if (openSessionId !== null && auditionableObjectIds.has(objectId)) {
            void auditionController.prefetch(openSessionId, objectId);
        }
    }

    function seekWaveData(item: WaveDataItem, ratio: number): void {
        void selectWaveData(item);
        if (auditionState.objectId !== item.objectKey) return;
        auditionController.seek(Math.floor(item.object.frameCount * ratio));
    }

    function resizeSplit(clientY: number): void {
        const bounds = mainStage.getBoundingClientRect();
        const available = Math.max(1, bounds.height - (auditionBarVisible ? 34 : 4));
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
        const available = Math.max(1, mainStage.getBoundingClientRect().height - (auditionBarVisible ? 34 : 4));
        const delta = (event.shiftKey ? 64 : 16) / available;
        splitRatio = Math.min(0.8, Math.max(0.2, splitRatio + (event.key === 'ArrowDown' ? delta : -delta)));
    }

    async function applyOpenedImage(
        opened: OpenedImage,
        preferred?: { partitionIndex: number; volumeName?: string },
    ): Promise<void> {
        clearPackageExportSelection();
        companionDirectories = opened.companionDirectories;
        volumeMutationsAvailable = opened.volumeMutationsAvailable;
        partitionMutationsAvailable = opened.partitionMutationsAvailable;
        objectRenameAvailable = opened.objectRenameAvailable;
        objectDeletionAvailable = opened.objectDeletionAvailable;
        waveDataCleanupAvailable = opened.waveDataCleanupAvailable;
        packageImportAvailable = opened.packageImportAvailable;
        packageExportAvailable = opened.packageExportAvailable;
        sourceItems = opened.tree;
        const preferredItem = preferred
            ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
            : null;
        selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? selectedSource;
        if (selectedSource.kind === 'volume') await loadVolume(selectedSource.id);
        else clearVolume();
        sourceStatus = opened.validation.valid ? 'Ready' : `${opened.validation.errorCount} validation errors`;
    }

    function currentSourcePreference(): { partitionIndex: number; volumeName?: string } | undefined {
        if (selectedSource.partitionIndex === undefined) return undefined;
        return {
            partitionIndex: selectedSource.partitionIndex,
            volumeName: selectedSource.kind === 'volume' ? selectedSource.name : selectedSource.volumeName,
        };
    }

    async function openImageLocation(
        location: ImageLocation,
        preferred?: { partitionIndex: number; volumeName?: string },
    ): Promise<void> {
        const generation = ++imageOpenGeneration;
        const previousSessionId = openSessionId;
        let candidateSessionId: number | null = null;
        imageOpening = true;
        sourceStatus = 'Opening image';
        try {
            const opened = await transport.openImage(location);
            candidateSessionId = opened.sessionId;
            if (generation !== imageOpenGeneration) {
                await transport.closeImage(opened.sessionId);
                candidateSessionId = null;
                return;
            }

            if (previousSessionId !== null) {
                await auditionController.invalidateSession(previousSessionId);
                await transport.closeImage(previousSessionId);
            }
            if (generation !== imageOpenGeneration) {
                await transport.closeImage(opened.sessionId);
                candidateSessionId = null;
                return;
            }

            imageLocation = location;
            openSessionId = opened.sessionId;
            candidateSessionId = null;
            await applyOpenedImage(opened, preferred);
        } catch (error) {
            if (generation !== imageOpenGeneration) return;
            if (candidateSessionId !== null) {
                await transport.closeImage(candidateSessionId).catch(() => undefined);
            }
            sourceStatus = userFacingMessage(error);
        } finally {
            if (generation === imageOpenGeneration) imageOpening = false;
        }
    }

    async function openSource(preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        if (imageLocation) await openImageLocation(imageLocation, preferred);
    }

    async function refreshOpenImageSession(preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        if (openSessionId === null) throw new Error('Image session is no longer available');
        const sessionId = openSessionId;
        const opened = await transport.refreshImage(sessionId);
        if (openSessionId !== sessionId) return;
        await applyOpenedImage(opened, preferred);
    }

    function requestCompanionDisks(retry: CompanionRetry): void {
        if (openSessionId === null || imageLocation?.kind !== 'axk-object-directory') return;
        companionDiskRequest = {
            directories: [...companionDirectories],
            retry,
            busy: false,
            error: '',
        };
    }

    function sameDirectory(left: DirectoryRef, right: DirectoryRef): boolean {
        return left.rootId === right.rootId && left.relativePath === right.relativePath;
    }

    async function addCompanionDiskFolder(): Promise<void> {
        const request = companionDiskRequest;
        if (!request || request.busy) return;
        const selection = await chooseServerLocation('directory', 'Choose companion disk folder', [], '', {
            parentDialog: 'companion-disks',
            initialDirectory: lastCompanionDirectory,
            ondirectorychange: (directory) => (lastCompanionDirectory = directory),
            requireWritableDirectory: false,
        });
        if (selection?.kind !== 'server-directory' || companionDiskRequest !== request) return;
        if (request.directories.some((directory) => sameDirectory(directory, selection.reference))) return;
        companionDiskRequest = {
            ...request,
            directories: [...request.directories, selection.reference],
            error: '',
        };
    }

    function removeCompanionDiskFolder(directory: DirectoryRef): void {
        if (!companionDiskRequest || companionDiskRequest.busy) return;
        companionDiskRequest = {
            ...companionDiskRequest,
            directories: companionDiskRequest.directories.filter((candidate) => !sameDirectory(candidate, directory)),
            error: '',
        };
    }

    async function retryCompanionAction(retry: CompanionRetry): Promise<void> {
        if (retry.kind === 'audition') {
            playObject(retry.objectId);
            return;
        }
        if (retry.kind === 'sample-bank') {
            const bank = sampleBanks.find((candidate) => candidate.objectId === retry.bankId);
            if (bank) await playSampleBank(bank);
            return;
        }
        await runPackageExport(retry.destination, retry.localDestination);
    }

    async function attachCompanionDisks(selection: CompanionDirectorySelection): Promise<void> {
        const request = companionDiskRequest;
        if (!request || request.busy || openSessionId === null) return;
        const sessionId = openSessionId;
        const preferred = currentSourcePreference();
        companionDiskRequest = { ...request, busy: true, error: '' };
        try {
            await auditionController.invalidateSession(sessionId);
            const opened = await transport.attachCompanionDirectories(sessionId, selection);
            if (openSessionId !== sessionId || companionDiskRequest?.retry !== request.retry) return;
            await applyOpenedImage(opened, preferred);
            companionDiskRequest = null;
            await retryCompanionAction(request.retry);
        } catch (error) {
            if (companionDiskRequest) {
                companionDiskRequest = {
                    ...companionDiskRequest,
                    busy: false,
                    error: userFacingMessage(error),
                };
            }
        }
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

    async function closeOpenImageSession(): Promise<void> {
        if (openSessionId === null) return;
        const sessionId = openSessionId;
        const packageRequest = packageImportRequest;
        ++packageOperationGeneration;
        packageImportAbortController?.abort();
        packageImportAbortController = null;
        packageImportRequest = null;
        packageExportRequest = null;
        companionDiskRequest = null;
        if (packageRequest) await releasePackageImportResources(packageRequest);
        await auditionController.invalidateSession(sessionId);
        await transport.closeImage(sessionId);
        clearPackageExportSelection();
        openSessionId = null;
        companionDirectories = [];
        pendingAuditionObjectId = '';
        volumeMutationsAvailable = false;
        partitionMutationsAvailable = false;
        objectRenameAvailable = false;
        objectDeletionAvailable = false;
        waveDataCleanupAvailable = false;
        objectRenameRequest = null;
        packageImportAvailable = false;
        packageExportAvailable = false;
        ++objectDeletionGeneration;
        objectDeletionRequest = null;
        ++waveDataCleanupGeneration;
        waveDataCleanupRequest = null;
    }

    async function closeImage(): Promise<void> {
        if (!imageLocation && openSessionId === null) return;
        ++imageOpenGeneration;
        imageOpening = false;
        sourceStatus = 'Closing image';
        try {
            await closeOpenImageSession();
            imageLocation = null;
            sourceItems = [];
            selectedSource = { id: 'none', name: 'No image', kind: 'disk', childCount: 0 };
            clearVolume();
            sourceStatus = 'Ready';
        } catch (error) {
            sourceStatus = userFacingMessage(error);
            throw error;
        }
    }

    async function chooseAndOpenSource(): Promise<void> {
        const selected = await chooseImageLocation();
        if (selected === null) return;
        await openImageLocation(selected);
    }

    async function chooseImageLocation(): Promise<ImageLocation | null> {
        if (transport.storageMode !== 'server') return null;
        const selection = await chooseServerLocation(
            'media-source',
            'Open image',
            ['hds', 'hda', 'ima', 'img', 'iso', 'a3k'],
            '',
            {
                initialDirectory: lastImageDirectory,
                ondirectorychange: (directory) => (lastImageDirectory = directory),
            },
        );
        return selection?.kind === 'server-file' || selection?.kind === 'axk-object-directory' ? selection : null;
    }

    async function chooseHardDiskCreationDirectory(): Promise<void> {
        if (transport.storageMode !== 'server') return;
        const selection = await chooseServerLocation('directory', 'Choose image location', [], '', {
            initialDirectory: lastImageDirectory,
            ondirectorychange: (directory) => (lastImageDirectory = directory),
        });
        if (selection?.kind === 'server-directory') hardDiskCreationDirectory = selection;
    }

    function suppressDesktopContextMenu(event: MouseEvent): void {
        if (isDesktop) event.preventDefault();
    }

    function chooseServerLocation(
        mode: PickerMode,
        title: string,
        extensions: string[] = [],
        suggestedName = '',
        navigation?: Pick<
            PickerRequest,
            'parentDialog' | 'initialDirectory' | 'ondirectorychange' | 'requireWritableDirectory'
        >,
    ): Promise<ImageLocation | DirectoryLocation | null> {
        return new Promise((resolve) => {
            pickerRequest?.resolve(null);
            pickerRequest = {
                mode,
                title,
                extensions,
                suggestedName,
                multiple: false,
                ...navigation,
                resolve: (selection) => resolve(Array.isArray(selection) ? null : selection),
            };
        });
    }

    function chooseServerFiles(
        title: string,
        extensions: string[],
        navigation?: Pick<
            PickerRequest,
            'parentDialog' | 'initialDirectory' | 'ondirectorychange' | 'requireWritableDirectory'
        >,
    ): Promise<FileLocation[] | null> {
        return new Promise((resolve) => {
            pickerRequest?.resolve(null);
            pickerRequest = {
                mode: 'file',
                title,
                extensions,
                suggestedName: '',
                multiple: true,
                ...navigation,
                resolve: (selection) => resolve(Array.isArray(selection) ? selection : null),
            };
        });
    }

    function finishPicker(selection: PickerSelection | null): void {
        const request = pickerRequest;
        pickerRequest = null;
        request?.resolve(selection);
    }

    function finishHardDiskCreation(file: FileLocation): void {
        hardDiskCreationDirectory = null;
        void openImageLocation(file);
    }

    async function openConnectionSettings(): Promise<void> {
        try {
            connectionSettings = await remoteServerSettings();
        } catch (error) {
            sourceStatus = userFacingMessage(error);
        }
    }

    async function saveRemoteConnection(input: RemoteServerSettingsInput): Promise<void> {
        await configureRemoteServer(input);
        window.location.reload();
    }

    async function switchToLocalConnection(): Promise<void> {
        await useLocalServer();
        window.location.reload();
    }
</script>

<svelte:head><title>axkdeck · A-series disk workspace</title></svelte:head>
<svelte:window
    oncontextmenu={suppressDesktopContextMenu}
    ondragenter={dragAudio}
    ondragover={dragAudio}
    ondragleave={leaveAudio}
    ondrop={dropAudio}
/>

<input
    bind:this={audioFileInput}
    class="sr-only"
    type="file"
    multiple
    accept={audioExtensions.map((extension) => `.${extension}`).join(',')}
    onchange={filesChosen}
/>

<div class:sidebar-closed={!sidebarOpen} class:inspector-closed={!inspectorOpen} class="app-shell">
    <header class="app-header">
        <div class="brand">
            <span class="brand-mark"><Icon name="waveform" size={20} strokeWidth={2.1} /></span><strong>axkdeck</strong>
        </div>
        <nav class="workspace-tabs" aria-label="Workspace views">
            {#each workspaceTabs as tab (tab.id)}
                <button
                    class:active={workspaceView === tab.id}
                    type="button"
                    onclick={() => selectWorkspaceView(tab.id)}
                >
                    <Icon name={tab.icon} size={16} /><span>{tab.label}</span>
                </button>
            {/each}
        </nav>
        {#if (packageExportAvailable || objectDeletionAvailable) && packageExportSelection.items.length > 0}
            <PackageSelectionControls
                count={packageExportSelection.items.length}
                onexport={packageExportAvailable
                    ? () => requestObjectPackageExport(packageExportSelection.items)
                    : undefined}
                ondelete={objectDeletionAvailable
                    ? () => requestObjectDeletion(packageExportSelection.items)
                    : undefined}
                onclear={clearPackageExportSelection}
            />
        {/if}
        <div class="global-controls">
            {#if isDesktop}
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Server connection settings"
                    title="Server connection settings"
                    onclick={() => void openConnectionSettings()}><Icon name="server" size={17} /></button
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
            onopen={() => void chooseAndOpenSource()}
            oncreate={() => void chooseHardDiskCreationDirectory()}
            onclose={() => void closeImage().catch(() => undefined)}
            onmanagelocations={() => (workspaceManagerOpen = true)}
            onselect={selectSource}
            volumeActionsEnabled={volumeMutationsAvailable}
            partitionActionsEnabled={partitionMutationsAvailable}
            packageImportEnabled={packageImportAvailable}
            packageExportEnabled={packageExportAvailable}
            onimageaction={requestImageAction}
            onloadchildren={(parentId, offset, limit) =>
                openSessionId === null
                    ? Promise.resolve({ items: [], totalCount: 0 })
                    : transport.contentChildren(openSessionId, parentId, offset, limit)}
        />
    {/if}

    <main
        bind:this={mainStage}
        class:lower-panel-closed={!lowerPanelOpen || !lowerPanelAvailable}
        class:has-audition-bar={auditionBarVisible}
        class="main-stage"
        style:--split-position={`${splitRatio * 100}%`}
        data-audio-drop-main={activeAudioTarget() ? 'true' : undefined}
    >
        {#if workspaceView === 'sample-banks' || workspaceView === 'samples'}
            <ContainedObjectWorkspace
                view={workspaceView}
                {sampleBanks}
                samples={workspaceView === 'sample-banks' ? bankMembers : samples}
                waveData={workspaceView === 'sample-banks' ? bankMemberWaveData : sampleWaveData}
                activeSampleBankId={workspaceView === 'sample-banks' ? selectedBankId : ''}
                activeSampleId={workspaceView === 'sample-banks' ? selectedBankMemberId : selectedSampleId}
                activeWaveDataId={workspaceView === 'sample-banks' ? selectedBankWaveDataId : selectedSampleWaveDataId}
                queries={laneQueries[workspaceView]}
                onquerychange={(lane, value) => updateLaneQuery(workspaceView, lane, value)}
                onsamplebankselect={selectBank}
                onsampleselect={workspaceView === 'sample-banks' ? selectBankMember : selectSample}
                onwavedataselect={selectWaveData}
                onplaysamplebank={playSampleBank}
                onplaysample={playSample}
                onplaywavedata={playContainedWaveData}
                onstop={() => void stopPlaybackNow()}
                onimportaudio={chooseAudioFiles}
                {playingSampleBankId}
                playingObjectId={auditionState.status === 'playing' ? auditionState.objectId : null}
                preparingObjectId={auditionState.status === 'preparing' ? auditionState.objectId : null}
                auditionableSampleIds={auditionableSampleObjectIds}
                auditionableSampleBankIds={auditionableSampleBankObjectIds}
                {objectRenameAvailable}
                onrenameobject={requestObjectRename}
                {objectDeletionAvailable}
                ondeleteobjects={requestObjectDeletion}
                {packageExportAvailable}
                onexportobjects={requestObjectPackageExport}
                selection={packageExportSelection}
                onselectionchange={(selection) => (packageExportSelection = selection)}
                onselectionlimit={reportPackageExportSelectionLimit}
            />
        {:else}
            <ObjectWorkspace
                {programs}
                {waveData}
                view={workspaceView}
                activeObjectId={activeCollectionObjectId}
                query={laneQueries[workspaceView].primary}
                onquerychange={(value) => updateLaneQuery(workspaceView, 'primary', value)}
                onprogramselect={selectProgram}
                onwavedataselect={selectWaveData}
                onpreviewrequest={requestWaveformPreview}
                onplay={playWaveData}
                onprefetch={(item) => prefetchObject(item.objectKey)}
                onstop={() => void stopPlaybackNow()}
                onseek={seekWaveData}
                playingObjectId={auditionState.status === 'playing' ? auditionState.objectId : null}
                preparingObjectId={auditionState.status === 'preparing' ? auditionState.objectId : null}
                playheadFrame={auditionState.playheadFrame}
                {objectRenameAvailable}
                onrenameobject={requestObjectRename}
                {objectDeletionAvailable}
                ondeleteobjects={requestObjectDeletion}
                waveDataCleanupAvailable={waveDataCleanupAvailable && activeVolumeId !== ''}
                oncleanupwavedata={requestWaveDataCleanup}
                {packageExportAvailable}
                onexportobjects={requestObjectPackageExport}
                selection={packageExportSelection}
                onselectionchange={(selection) => (packageExportSelection = selection)}
                onselectionlimit={reportPackageExportSelectionLimit}
            />
        {/if}
        <AuditionBar
            available={auditionAvailable}
            {autoplay}
            state={auditionState}
            label={auditionLabel}
            onautoplaychange={(enabled) => (autoplay = enabled)}
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
                assignmentQuery={laneQueries.programs.secondary}
                onassignmentquerychange={(value) => (laneQueries.programs.secondary = value)}
                onassignmentselect={selectAssignment}
            />
        {/if}
    </main>

    {#if inspectorOpen}
        <ObjectInspector
            selection={inspectorSelection}
            playingObjectId={auditionState.status === 'playing' ? auditionState.objectId : null}
            playheadFrame={auditionState.playheadFrame}
        />
    {/if}
    <footer class="status-bar">
        <span><span class="status-dot"></span> {sourceStatus}</span><span class="ml-auto"
            >{sourceObjectCount} objects</span
        >
    </footer>
</div>

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
            workspaceManagerOpen = true;
        }}
        onselect={(selection) => finishPicker(selection)}
        onselectmany={(selections) => finishPicker(selections)}
        oncancel={() => finishPicker(null)}
    />
{/if}
{#if companionDiskRequest && pickerRequest?.parentDialog !== 'companion-disks'}
    <CompanionDiskDialog
        directories={companionDiskRequest.directories}
        busy={companionDiskRequest.busy}
        error={companionDiskRequest.error}
        onadd={() => void addCompanionDiskFolder()}
        onremove={removeCompanionDiskFolder}
        onnearby={() => void attachCompanionDisks({ kind: 'immediate-siblings' })}
        onconfirm={() =>
            void attachCompanionDisks({
                kind: 'directories',
                directories: companionDiskRequest?.directories ?? [],
            })}
        oncancel={() => !companionDiskRequest?.busy && (companionDiskRequest = null)}
    />
{/if}
{#if hardDiskCreationDirectory}
    <CreateHardDiskImageDialog
        {transport}
        directory={hardDiskCreationDirectory}
        onsuccess={finishHardDiskCreation}
        oncancel={() => (hardDiskCreationDirectory = null)}
    />
{/if}
<WorkspaceManager
    open={workspaceManagerOpen}
    activeWorkspaceId={imageLocation?.reference.rootId ?? null}
    onclose={() => (workspaceManagerOpen = false)}
/>
{#if connectionSettings}
    <ServerConnectionSettings
        settings={connectionSettings}
        onsave={saveRemoteConnection}
        onuselocal={switchToLocalConnection}
        oncancel={() => (connectionSettings = null)}
    />
{/if}
{#if volumeAction}
    {#key `${volumeAction.action}:${volumeAction.item.id}`}
        <VolumeActionDialog
            action={volumeAction.action}
            item={volumeAction.item}
            busy={volumeActionBusy}
            error={volumeActionError}
            oncancel={() => !volumeActionBusy && (volumeAction = null)}
            onsubmit={(name) => void submitVolumeAction(name)}
        />
    {/key}
{/if}
{#if objectRenameRequest}
    {#key objectRenameRequest.target.object.key}
        <ObjectRenameDialog
            target={objectRenameRequest.target}
            busy={objectRenameRequest.busy}
            error={objectRenameRequest.error}
            oncancel={() => !objectRenameRequest?.busy && (objectRenameRequest = null)}
            onsubmit={(name) => void submitObjectRename(name)}
        />
    {/key}
{/if}
{#if packageImportRequest && pickerRequest?.parentDialog !== 'package-import'}
    <PackageImportDialog
        targetName={packageImportRequest.item.name}
        desktop={isDesktop}
        sourceName={packageImportRequest.sourceName}
        inspection={packageImportRequest.inspection}
        plan={packageImportRequest.plan}
        renames={packageImportRequest.renames}
        status={packageImportRequest.status}
        progress={packageImportRequest.progress}
        error={packageImportRequest.error}
        onchooseworkspace={() => void chooseWorkspacePackage()}
        onchooselocal={() => void chooseLocalPackage()}
        onchange={() => void resetPackageImportSource()}
        onrename={(nodeId, name) => {
            if (packageImportRequest) {
                packageImportRequest = {
                    ...packageImportRequest,
                    renames: { ...packageImportRequest.renames, [nodeId]: name },
                };
            }
        }}
        onreplan={() => void replanPackageImport()}
        oncancel={() => void closePackageImport()}
        onconfirm={() => void applyPackageImport()}
    />
{/if}
{#if packageExportRequest && pickerRequest?.parentDialog !== 'package-export' && !companionDiskRequest}
    <PackageExportDialog
        items={packageExportRequest.items}
        desktop={isDesktop}
        busy={packageExportRequest.busy}
        progressLabel={packageExportRequest.progressLabel}
        error={packageExportRequest.error}
        onworkspace={() => void exportPackageToWorkspace()}
        onlocal={() => void exportPackageToComputer()}
        oncancel={() => {
            if (!packageExportRequest?.busy) {
                ++packageOperationGeneration;
                packageExportRequest = null;
            }
        }}
    />
{/if}
{#if objectDeletionRequest}
    <ObjectDeletionDialog
        inspection={objectDeletionRequest.inspection}
        loading={objectDeletionRequest.loading}
        busy={objectDeletionRequest.busy}
        error={objectDeletionRequest.error}
        onselectionchange={updateObjectDeletionSelection}
        onselectall={updateAllObjectDeletionDependencies}
        oncancel={cancelObjectDeletion}
        onconfirm={() => void submitObjectDeletion()}
    />
{/if}
{#if waveDataCleanupRequest}
    <WaveDataCleanupDialog
        volumeName={waveDataCleanupRequest.volumeName}
        inspection={waveDataCleanupRequest.inspection}
        selectedObjectIds={waveDataCleanupRequest.selectedObjectIds}
        loading={waveDataCleanupRequest.loading}
        busy={waveDataCleanupRequest.busy}
        error={waveDataCleanupRequest.error}
        onselectionchange={updateWaveDataCleanupSelection}
        onselectall={updateAllWaveDataCleanupSelection}
        oncancel={cancelWaveDataCleanup}
        onconfirm={() => void submitWaveDataCleanup()}
    />
{/if}
{#if audioImportRequest && pickerRequest?.parentDialog !== 'audio-import'}
    <AudioImportDialog
        {transport}
        files={audioImportRequest.files}
        target={audioImportRequest.target}
        existingSampleNames={samples.map((item) => item.name)}
        existingWaveformNames={waveData.map((item) => item.name)}
        onchooseworkspace={() => void chooseWorkspaceAudio()}
        onchooselocal={transport.supportsClientUploads ? chooseLocalAudio : undefined}
        oncommit={commitAudioImport}
        oncancel={() => (audioImportRequest = null)}
    />
{/if}
{#if audioDragActive && !audioImportRequest}
    <div class="audio-drop-overlay" aria-hidden="true">
        <Icon name="upload" size={24} />
        <strong
            >{audioDragTarget ? `Import audio into ${audioDragTarget.volumeName}` : 'Select a writable volume'}</strong
        >
        <span>WAV, FLAC, and AIFF</span>
    </div>
{/if}
