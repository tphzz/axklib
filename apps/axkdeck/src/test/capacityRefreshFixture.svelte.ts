import { CatalogWorkflow } from '../features/catalog/workflow.svelte';
import { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
import type { PickerController } from '../features/dialogs/picker';
import type { WorkspaceProps } from '../features/backends/axklib/workspaceProps';
import type { DiskTreeItem } from '../lib/types';
import type { ImageTransport, OpenedImage, VolumeCapacityInspection } from '../lib/transport';
import { inspectorRelationshipFixture } from './inspectorRelationshipFixture';
import { volumeCapacityFixture } from './volumeCapacityFixture';

export function capacityRefreshFixture() {
    let revision = 1;
    let name = 'foo';
    let sameId = false;
    let scenario: 'FITS' | 'DOES_NOT_FIT' | 'ERROR' = 'FITS';
    let hold = $state(false);
    let calls = $state<string[]>([]);
    let pending = $state<{ result: VolumeCapacityInspection; resolve: (result: VolumeCapacityInspection) => void }[]>(
        [],
    );
    let objectSelected = $state(false);
    const noop = () => {};
    const selection = inspectorRelationshipFixture('program');
    if (selection?.kind !== 'program') throw new Error('Expected Program fixture');
    const volume = (): DiskTreeItem => ({
        id: sameId ? 'volume-1' : `volume-${revision}`,
        name,
        kind: 'volume',
        partitionIndex: 0,
        volumeDirectoryId: 9,
        childCount: 0,
        sizeBytes: revision * 1024,
    });
    const opened = (): OpenedImage => ({
        sessionId: 1,
        revision,
        companionSources: [],
        floppySet: null,
        format: 'sfs',
        tree: [
            {
                id: `partition-${revision}`,
                name: 'PARTITION 1',
                kind: 'partition',
                partitionIndex: 0,
                childCount: 1,
                children: [volume()],
            },
        ],
        initialVolume: volume(),
        objects: [],
        objectTotalCount: 0,
        validation: {
            valid: true,
            issueCount: 0,
            errorCount: 0,
            warningCount: 0,
            objectCount: 1,
            relationshipCount: 0,
        },
        volumeMutationsAvailable: false,
        partitionMutationsAvailable: false,
        objectRenameAvailable: false,
        objectDeletionAvailable: false,
        waveDataCleanupAvailable: false,
        programGenerationAvailable: false,
        programAssignmentCleanupAvailable: false,
        packageImportAvailable: true,
        packageExportAvailable: false,
        volumePackageExportAvailable: false,
        volumeFloppyExportAvailable: false,
        audioExportAvailable: false,
        sequenceExportAvailable: false,
        mediaConversionAvailable: false,
        extentLayoutRepairAvailable: false,
        allocationInspectionAvailable: false,
    });
    const transport = {
        storageMode: 'server',
        openImage: async () => opened(),
        refreshImage: async () => opened(),
        closeImage: async () => {},
        keepImageAlive: async () => {},
        objectPage: async () => ({ objects: [selection.program.object], totalCount: 1 }),
        relationshipPage: async () => ({ relationships: [], totalCount: 0 }),
        contentChildren: async () => ({ items: [], totalCount: 0 }),
        systemProgramContexts: async () => ({ partitionIndex: 0, files: [], message: '' }),
        filesystem: async () => ({
            revision,
            available: false,
            deviceView: 'a-series',
            filesystemName: 'SFS',
            items: [],
            totalCount: 0,
            rootCapabilities: [],
        }),
        inspectVolumeCapacity: async (_session: number, contentScopeId: string) => {
            calls.push(`${revision}:${contentScopeId}`);
            if (contentScopeId !== volume().id)
                throw new Error('Choose an A-Series SFS volume for capacity inspection');
            if (scenario === 'ERROR') throw new Error('Capacity service disconnected');
            const report = volumeCapacityFixture(scenario);
            report.volumeName = name;
            for (const profile of report.profiles) profile.peakBytes = 124000 + revision * 1024;
            const result = { imageId: 'image', revision, contentScopeId, report };
            if (!hold) return result;
            return new Promise<VolumeCapacityInspection>((resolve) => pending.push({ result, resolve }));
        },
    } as unknown as ImageTransport;
    const workflow = new ImageSessionWorkflow(transport, {} as PickerController);
    const catalog = new CatalogWorkflow({
        transport,
        sessionId: () => workflow.sessionId,
        stopPlayback: async () => {},
        resetPreviews: noop,
        resetCleanup: noop,
        setStatus: noop,
    });
    const audition = {
        laneQueries: { programs: { primary: '', secondary: '', tertiary: '' } },
        state: { status: 'idle' },
        invalidateSession: async () => {},
        selectProgram: () => {
            objectSelected = true;
        },
    } as unknown as WorkspaceProps['audition'];
    const mutation = {
        volumeAvailable: false,
        partitionAvailable: false,
        objectRenameAvailable: false,
        setCapabilities: noop,
    } as unknown as WorkspaceProps['mutation'];
    workflow.connect({ catalog, audition, mutation, clearExportSelection: noop } as never);
    return {
        workflow,
        get calls() {
            return calls;
        },
        get pendingCount() {
            return pending.length;
        },
        get hold() {
            return hold;
        },
        set hold(value: boolean) {
            hold = value;
        },
        async open() {
            await workflow.open({
                kind: 'server-file',
                displayName: 'test.hds',
                reference: { rootId: 'root', relativePath: 'test.hds' },
            });
        },
        async refresh(next: typeof scenario = 'FITS', destination = 'foo', retainId = false) {
            revision++;
            scenario = next;
            name = destination;
            sameId = retainId;
            await workflow.refresh({ partitionIndex: 0, volumeName: name });
        },
        release() {
            const queued = pending;
            pending = [];
            for (const request of queued) request.resolve(request.result);
        },
        recover() {
            scenario = 'FITS';
        },
        get props(): WorkspaceProps {
            return {
                transport,
                isDesktop: false,
                workspaceTabs: [{ id: 'programs', label: 'Programs', icon: 'music' }],
                workspaceView: 'programs',
                revision: workflow.revision,
                imageLocation: workflow.location,
                sourceItems: workflow.sourceItems,
                selectedSource: workflow.selectedSource,
                selectedVolumeIds: workflow.volumeSelection.items.map((item) => item.id),
                imageOpening: false,
                sessionId: workflow.sessionId,
                catalog,
                audition,
                mutation,
                audioImport: {} as WorkspaceProps['audioImport'],
                sequenceImport: {} as WorkspaceProps['sequenceImport'],
                importAudio: noop,
                importMidi: noop,
                programs: catalog.programs,
                sampleBanks: [],
                samples: [],
                waveData: [],
                sequences: [],
                bankMembers: [],
                bankMemberWaveData: [],
                sampleWaveData: [],
                activeCollectionObjectId: '',
                inspectorSelection: objectSelected ? selection : null,
                editorSelection: null,
                sourceStatus: '',
                packageSelection: { items: [], anchors: {} },
                objectDeletionAvailable: false,
                waveDataCleanupAvailable: false,
                programGenerationAvailable: false,
                programAssignmentCleanupAvailable: false,
                packageImportAvailable: true,
                packageExportAvailable: false,
                volumePackageExportAvailable: false,
                volumeFloppyExportAvailable: false,
                audioExportAvailable: false,
                sequenceExportAvailable: false,
                mediaConversionAvailable: false,
                allocationInspectionAvailable: false,
                samplerOrderingEnabled: true,
                openConnectionSettings: noop,
                openImage: noop,
                createImage: noop,
                closeImage: noop,
                showImageIntegrity: noop,
                manageLocations: noop,
                selectSource: (item, mode, visible) => workflow.selectTreeSource(item, mode, visible),
                selectSourceForContext: (item, visible) => workflow.selectSourceForContext(item, visible),
                imageAction: noop,
                selectWorkspace: noop,
                exportPackage: noop,
                exportAudio: noop,
                exportWav: noop,
                exportMidi: noop,
                deleteObjects: noop,
                cleanupWaveData: noop,
                generatePrograms: noop,
                cleanupProgramAssignments: noop,
                clearSelection: noop,
                selectionChanged: noop,
                selectionLimit: noop,
                setStatus: noop,
            };
        },
    };
}
