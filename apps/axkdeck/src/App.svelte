<script lang="ts">
    import { onDestroy, onMount } from 'svelte';
    import AuditionBar from './lib/components/AuditionBar.svelte';
    import ContainedObjectWorkspace from './lib/components/ContainedObjectWorkspace.svelte';
    import Icon from './lib/components/Icon.svelte';
    import ImageNavigator from './lib/components/ImageNavigator.svelte';
    import LayoutControls from './lib/components/LayoutControls.svelte';
    import ObjectInspector from './lib/components/ObjectInspector.svelte';
    import ObjectEditor from './lib/components/ObjectEditor.svelte';
    import ObjectWorkspace from './lib/components/ObjectWorkspace.svelte';
    import ServerConnectionSettings from './lib/components/ServerConnectionSettings.svelte';
    import CreateHardDiskImageDialog from './lib/components/CreateHardDiskImageDialog.svelte';
    import AudioImportDialog from './lib/components/AudioImportDialog.svelte';
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
    import { audioExtensions, isSupportedAudioFile } from './lib/audioImport';
    import { browserUploadSource, type ClientUploadSource } from './lib/clientUploadSource';
    import { reportDiagnostic, reportError } from './lib/diagnostics';
    import { listenForNativeAudioDrops, type NativeDropPosition } from './lib/nativeAudioDrop';
    import { collectPages } from './lib/pagination';
    import { type DirectoryLocation, type DirectoryRef, type FileLocation } from './lib/storageLocations';
    import type {
        AudioImportItem,
        AudioImportTarget,
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
        Program,
        ProgramAssignmentRow,
        SampleStructureItem,
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

    const transport = createTransport();
    const isDesktop = '__TAURI_INTERNALS__' in window;
    type PickerMode = 'file' | 'directory' | 'save-file';
    interface PickerRequest {
        mode: PickerMode;
        title: string;
        extensions: string[];
        suggestedName: string;
        initialDirectory?: DirectoryRef | null;
        ondirectorychange?: (directory: DirectoryRef | null) => void;
        resolve: (selection: FileLocation | DirectoryLocation | null) => void;
    }

    interface LaneQueries {
        primary: string;
        secondary: string;
        tertiary: string;
    }

    let sourceItems = $state<DiskTreeItem[]>([]);
    let selectedSource = $state<DiskTreeItem>({ id: 'none', name: 'No image', kind: 'disk', childCount: 0 });
    let imageLocation = $state<FileLocation | null>(null);
    let pickerRequest = $state<PickerRequest | null>(null);
    let hardDiskCreationDirectory = $state<DirectoryLocation | null>(null);
    let lastImageDirectory = $state<DirectoryRef | null>(null);
    let openSessionId = $state<number | null>(null);
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
    let lowerPanelOpen = $state(true);
    let inspectorOpen = $state(true);
    let splitRatio = $state(2 / 3);
    let mainStage: HTMLElement;
    let resizing = $state(false);
    let connectionSettings = $state<RemoteServerSettingsView | null>(null);
    let workspaceManagerOpen = $state(false);
    let volumeMutationsAvailable = $state(false);
    let partitionMutationsAvailable = $state(false);
    let volumeAction = $state<{ item: DiskTreeItem; action: ImageTreeAction } | null>(null);
    let volumeActionBusy = $state(false);
    let volumeActionError = $state('');
    let audioFileInput: HTMLInputElement;
    let audioImportRequest = $state<{ files: ClientUploadSource[]; target: AudioImportTarget } | null>(null);
    let audioDragActive = $state(false);
    let audioDragTarget = $state<AudioImportTarget | null>(null);
    let activeVolumeId = $state('');
    let volumeLoadGeneration = 0;
    let auditionState = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    let autoplay = $state(false);
    let playingSampleBankId = $state('');
    let sampleBankPreviewMemberId = $state('');
    let sampleBankPlaybackGeneration = 0;
    const previewQueue: { item: WaveDataItem; generation: number }[] = [];
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
        if (state.status === 'failed' && state.error) sourceStatus = state.error;
    });

    onDestroy(() => {
        stopInterfaceScaleSubscription?.();
        void interfaceScaling?.dispose();
        ++imageOpenGeneration;
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

    async function requestAudioImport(files: ClientUploadSource[], target = activeAudioTarget()): Promise<void> {
        const admitted = files.filter(isSupportedAudioFile);
        if (files.length > 0 && admitted.length === 0) {
            sourceStatus = 'No supported audio files were dropped';
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
        if (!activeAudioTarget()) {
            sourceStatus = 'Select a writable volume first';
            return;
        }
        audioFileInput.click();
    }

    function filesChosen(event: Event): void {
        const input = event.currentTarget as HTMLInputElement;
        void requestAudioImport(Array.from(input.files ?? []).map(browserUploadSource));
        input.value = '';
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
        if (!audioImportRequest || !imageLocation) throw new Error('Audio import target is no longer available');
        const target = audioImportRequest.target;
        const firstName = items[0]?.sampleName;
        sourceStatus = 'Importing audio';
        try {
            await closeOpenImageSession();
            const job = await transport.startAudioImport(imageLocation, target, items);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Audio import did not complete');
            selectWorkspaceView('samples');
            await openSource(target);
            const inserted = samples.find((sample) => sample.name === firstName);
            if (inserted) selectSample(inserted);
        } catch (error) {
            if (openSessionId === null) await openSource(target);
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
                memberPreviews: members.map((item) => ({
                    item,
                    waveData: linkedWaveDataForSample(item.objectId, relationships, waveData),
                })),
                displayedMemberId,
            };
        }
        const sample = samples.find((item) => item.objectId === objectId);
        if (sample) {
            return {
                kind: 'sample',
                item: sample,
                memberships: banksForSample(sample.objectId),
                waveData: linkedWaveDataForSample(sample.objectId, relationships, waveData),
            };
        }
        const waveform = waveData.find((item) => item.objectKey === objectId);
        return waveform ? { kind: 'wave-data', waveData: waveform } : null;
    }

    $effect(() => {
        const previews =
            inspectorSelection?.kind === 'sample'
                ? [inspectorSelection.waveData]
                : inspectorSelection?.kind === 'sample-bank'
                  ? inspectorSelection.memberPreviews.map((member) => member.waveData)
                  : [];
        for (const preview of previews) {
            for (const member of preview) requestWaveformPreview(member.waveData);
        }
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
        const partitionAction = action === 'rename-partition';
        if (partitionAction && (!partitionMutationsAvailable || item.kind !== 'partition')) return;
        if (!partitionAction && !volumeMutationsAvailable) return;
        if (action === 'add-volume' && item.kind !== 'partition') return;
        if ((action === 'rename-volume' || action === 'delete-volume') && item.kind !== 'volume') return;
        selectedSource = item;
        volumeActionError = '';
        volumeAction = { item, action };
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
        try {
            await closeOpenImageSession();
            const job = partitionMutation
                ? await transport.startPartitionMutation(imageLocation, partitionMutation)
                : await transport.startVolumeMutation(imageLocation, volumeMutation!);
            const completed = await transport.waitForJob(job.jobId, (update) => {
                if (update.progress?.label) sourceStatus = update.progress.label;
            });
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Image change did not complete');
            }
            volumeAction = null;
            await openSource({ partitionIndex, volumeName: preferredVolumeName });
        } catch (error) {
            volumeActionError = userFacingMessage(error);
            sourceStatus = volumeActionError;
            if (openSessionId === null) {
                await openSource({ partitionIndex, volumeName: previousVolumeName });
            }
        } finally {
            volumeActionBusy = false;
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
        previewQueue.push({ item, generation: previewGeneration });
        drainPreviewQueue();
    }

    function drainPreviewQueue(): void {
        while (previewInflight < 2 && previewQueue.length > 0 && openSessionId !== null) {
            const queued = previewQueue.shift();
            if (!queued) return;
            const { item, generation } = queued;
            const sessionId = openSessionId;
            previewInflight += 1;
            void transport
                .preview(sessionId, item.objectKey, 1024)
                .then((preview) => {
                    if (openSessionId !== sessionId || previewGeneration !== generation) return;
                    waveData = waveData.map((candidate) =>
                        candidate.id === item.id
                            ? { ...candidate, waveform: preview.bins, previewState: 'ready' }
                            : candidate,
                    );
                })
                .catch((error) => {
                    if (openSessionId !== sessionId || previewGeneration !== generation) return;
                    previewFailed.add(item.objectKey);
                    waveData = waveData.map((candidate) =>
                        candidate.id === item.id ? { ...candidate, previewState: 'failed' } : candidate,
                    );
                    sourceStatus = userFacingMessage(error);
                })
                .finally(() => {
                    if (previewGeneration === generation) previewPending.delete(item.objectKey);
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
        const memberIds = membersForBank(item.objectId)
            .filter((member) => auditionableSampleObjectIds.has(member.objectId))
            .map((member) => member.objectId);
        if (memberIds.length === 0) {
            sampleBankPreviewMemberId = '';
            sourceStatus = 'This Sample Bank has no playable Samples';
            return;
        }
        const generation = ++sampleBankPlaybackGeneration;
        playingSampleBankId = item.objectId;
        sampleBankPreviewMemberId = memberIds[0] ?? '';
        auditionController.playSequence(openSessionId, memberIds, ({ playedCount }) => {
            if (generation !== sampleBankPlaybackGeneration) return;
            playingSampleBankId = '';
            resetSampleBankPreview(item.objectId);
            if (playedCount === 0) sourceStatus = 'This Sample Bank has no playable Samples';
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

    async function openImageLocation(
        location: FileLocation,
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
            volumeMutationsAvailable = opened.volumeMutationsAvailable;
            partitionMutationsAvailable = opened.partitionMutationsAvailable;
            sourceItems = opened.tree;
            const preferredItem = preferred
                ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
                : null;
            selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? selectedSource;
            if (selectedSource.kind === 'volume') await loadVolume(selectedSource.id);
            else clearVolume();
            sourceStatus = opened.validation.valid ? 'Ready' : `${opened.validation.errorCount} validation errors`;
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

    async function closeOpenImageSession(): Promise<void> {
        if (openSessionId === null) return;
        const sessionId = openSessionId;
        await auditionController.invalidateSession(sessionId);
        await transport.closeImage(sessionId);
        openSessionId = null;
        volumeMutationsAvailable = false;
        partitionMutationsAvailable = false;
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

    async function chooseImageLocation(): Promise<FileLocation | null> {
        if (transport.storageMode !== 'server') return null;
        const selection = await chooseServerLocation(
            'file',
            'Open image',
            ['hds', 'hda', 'ima', 'img', 'iso', 'a3k'],
            '',
            {
                initialDirectory: lastImageDirectory,
                ondirectorychange: (directory) => (lastImageDirectory = directory),
            },
        );
        return selection?.kind === 'server-file' ? selection : null;
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
        navigation?: Pick<PickerRequest, 'initialDirectory' | 'ondirectorychange'>,
    ): Promise<FileLocation | DirectoryLocation | null> {
        return new Promise((resolve) => {
            pickerRequest?.resolve(null);
            pickerRequest = { mode, title, extensions, suggestedName, ...navigation, resolve };
        });
    }

    function finishPicker(selection: FileLocation | DirectoryLocation | null): void {
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
        initialDirectory={pickerRequest.initialDirectory}
        ondirectorychange={pickerRequest.ondirectorychange}
        onmanagelocations={() => {
            finishPicker(null);
            workspaceManagerOpen = true;
        }}
        onselect={(selection) => finishPicker(selection)}
        oncancel={() => finishPicker(null)}
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
<WorkspaceManager open={workspaceManagerOpen} onclose={() => (workspaceManagerOpen = false)} />
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
{#if audioImportRequest}
    <AudioImportDialog
        {transport}
        files={audioImportRequest.files}
        target={audioImportRequest.target}
        existingSampleNames={samples.map((item) => item.name)}
        existingWaveformNames={waveData.map((item) => item.name)}
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
