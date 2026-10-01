import { describe, expect, it, vi } from 'vitest';
import type {
    ContentPage,
    ImageTransport,
    ObjectPage,
    RelationshipPage,
    SamplerObject,
    SamplerRelationship,
    SystemProgramContexts,
} from '../../lib/transport';
import type { Program, SampleStructureItem } from '../../lib/types';
import { CatalogWorkflow } from './workflow.svelte';

interface Deferred<T> {
    promise: Promise<T>;
    resolve: (value: T) => void;
    reject: (error: unknown) => void;
}

interface VolumeRequests {
    objects: Deferred<ObjectPage>;
    relationships: Deferred<RelationshipPage>;
    names: Deferred<ContentPage>;
    contexts: Deferred<SystemProgramContexts>;
}

function deferred<T>(): Deferred<T> {
    let resolve!: (value: T) => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<T>((promiseResolve, promiseReject) => {
        resolve = promiseResolve;
        reject = promiseReject;
    });
    return { promise, resolve, reject };
}

function programObject(key: string, name: string): SamplerObject {
    return {
        key,
        objectType: 'PROG',
        name,
        partitionIndex: 0,
        partitionName: 'Partition',
        volumeName: 'Volume',
        categoryName: '',
        objectEncoding: 'current',
        directoryEntryName: `${name}.001`,
        sfsId: 1,
        storedSizeBytes: 1,
        sizeWithDependenciesBytes: null,
        sampleRate: 0,
        rootKey: 0,
        storedFrameCount: 0,
        waveStartFrame: 0,
        waveLengthFrames: 0,
        storageState: 'COMPLETE',
        sampleWidthBytes: 0,
    };
}

function context(partitionIndex: number): SystemProgramContexts {
    return {
        partitionIndex,
        files: [
            {
                fileKind: 'SYSTEM',
                availability: 'NOT_PRESENT',
                message: `No saved SYSTEM file exists for partition ${partitionIndex}.`,
            },
            {
                fileKind: 'SYSTEM2',
                availability: 'NOT_PRESENT',
                message: `No saved SYSTEM2 file exists for partition ${partitionIndex}.`,
            },
        ],
        message: '',
    };
}

function volumeRequests(): VolumeRequests {
    return {
        objects: deferred(),
        relationships: deferred(),
        names: deferred(),
        contexts: deferred(),
    };
}

function resolveVolumeData(requests: VolumeRequests, object: SamplerObject, visibleName: string): void {
    requests.objects.resolve({ objects: [object], totalCount: 1 });
    requests.relationships.resolve({ relationships: [], totalCount: 0 });
    requests.names.resolve({
        items: [
            {
                id: `content-${object.key}`,
                name: visibleName,
                kind: 'object',
                childCount: 0,
                objectId: object.key,
                objectType: object.objectType,
            },
        ],
        totalCount: 1,
    });
}

function workflowHarness(
    requestsByVolume: Map<string, VolumeRequests>,
    requestsByPartition: Map<number, VolumeRequests>,
) {
    const statuses: string[] = [];
    const transport = {
        objectPage: vi.fn(
            (_sessionId, _offset, _limit, filter) => requestsByVolume.get(filter.scopeId)!.objects.promise,
        ),
        relationshipPage: vi.fn(
            (_sessionId, _offset, _limit, filter) => requestsByVolume.get(filter.scopeId)!.relationships.promise,
        ),
        contentChildren: vi.fn((_sessionId, parentId) => requestsByVolume.get(parentId)!.names.promise),
        systemProgramContexts: vi.fn(
            (_sessionId, partitionIndex) => requestsByPartition.get(partitionIndex)!.contexts.promise,
        ),
    } as unknown as ImageTransport;
    const workflow = new CatalogWorkflow({
        transport,
        sessionId: () => 1,
        stopPlayback: () => Promise.resolve(),
        resetPreviews: () => undefined,
        resetCleanup: () => undefined,
        setStatus: (status) => statuses.push(status),
    });
    return { workflow, statuses };
}

async function flushPromises(): Promise<void> {
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
    await Promise.resolve();
}

describe('CatalogWorkflow volume snapshots', () => {
    it('counts unresolved bank references, not their expanded candidate rows', () => {
        const { workflow } = workflowHarness(new Map(), new Map());
        const item = (id: string, objectType: 'SBAC' | 'SBNK'): SampleStructureItem => ({
            id,
            objectId: id,
            name: id,
            objectType,
            object: { ...programObject(id, id), objectType },
        });
        workflow.sampleBanks = [item('bank', 'SBAC')];
        workflow.samples = [item('known', 'SBNK')];
        const edge = (id: string, values: Partial<SamplerRelationship> = {}): SamplerRelationship => ({
            id,
            sourceObjectId: 'bank',
            relationshipType: 'SBAC_SLOT_TO_SBNK',
            quality: 'KNOWN',
            targetObjectId: 'known',
            candidateObjectIds: [],
            basis: 'test',
            notes: [],
            assignmentName: '',
            assignmentState: '',
            receiveChannelDisplay: '',
            ...values,
        });
        workflow.relationships = [
            edge('resolved'),
            edge('ambiguous', {
                targetObjectId: undefined,
                candidateObjectIds: ['candidate1', 'candidate2'],
                quality: 'UNKNOWN',
            }),
        ];
        const selection = workflow.selectionForObject('bank');
        expect(selection?.kind).toBe('sample-bank');
        if (selection?.kind !== 'sample-bank') throw new Error('Expected bank');
        expect(selection.members.map((member) => member.objectId)).toEqual(['known']);
        expect(selection.relationships?.find((group) => group.objectType === 'SBNK')?.items).toHaveLength(3);
        expect(selection.unresolvedMemberCount).toBe(1);
        workflow.relationships = [...workflow.relationships, edge('missing', { targetObjectId: 'missing' })];
        expect(workflow.selectionForObject('bank')).toMatchObject({ unresolvedMemberCount: 2 });
    });
    it('adopts a refreshed scope ID only after loading succeeds, retaining object selections', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['refreshed', requests]]), new Map([[0, requests]]));
        workflow.activeVolumeId = 'previous';
        workflow.activePartitionIndex = 0;
        workflow.programs = [{ objectId: 'program', name: 'Old' } as Program];
        workflow.selectedProgramId = 'program';
        const before = workflow.programs;
        const loading = workflow.refreshVolume('previous', 'refreshed', 0);
        expect(workflow.activeVolumeId).toBe('previous');
        expect(workflow.programs).toBe(before);
        resolveVolumeData(requests, programObject('program', '001'), '001: Current');
        requests.contexts.resolve(context(0));
        await loading;
        expect(workflow.activeVolumeId).toBe('refreshed');
        expect(workflow.selectedProgramId).toBe('program');
        expect(workflow.programs[0]?.name).toBe('Current');
    });

    it('does not adopt a refreshed scope ID when loading fails', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['refreshed', requests]]), new Map([[0, requests]]));
        workflow.activeVolumeId = 'previous';
        workflow.activePartitionIndex = 0;
        const loading = workflow.refreshVolume('previous', 'refreshed', 0);
        requests.objects.reject(new Error('Read failed'));
        await expect(loading).rejects.toThrow('Read failed');
        expect(workflow.activeVolumeId).toBe('previous');
    });

    it('keeps current data and the latest selections until a same-volume refresh completes', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['volume', requests]]), new Map([[0, requests]]));
        workflow.activeVolumeId = 'volume';
        workflow.activePartitionIndex = 0;
        workflow.programs = [{ objectId: 'program', name: 'Old' } as Program];
        workflow.selectedProgramId = 'program';
        workflow.inspectorObjectId = 'deleted';
        workflow.editorObjectIds.programs = 'program';
        const before = workflow.programs;
        const loading = workflow.refreshVolume('volume', 'volume', 0);
        expect(workflow.programs).toBe(before);
        expect(workflow.selectedProgramId).toBe('program');
        workflow.selectedProgramId = '';
        resolveVolumeData(requests, programObject('program', '001'), '001: Current');
        requests.contexts.resolve(context(0));
        await loading;
        expect(workflow.programs[0]?.name).toBe('Current');
        expect(workflow.selectedProgramId).toBe('');
        expect(workflow.inspectorObjectId).toBe('');
        expect(workflow.editorObjectIds.programs).toBe('program');
    });

    it('retains the catalog on refresh failure and exposes the failure to Save recovery', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['volume', requests]]), new Map([[0, requests]]));
        workflow.activeVolumeId = 'volume';
        workflow.activePartitionIndex = 0;
        workflow.programs = [{ objectId: 'program', name: 'Old' } as Program];
        workflow.selectedProgramId = 'program';
        const before = workflow.programs;
        const loading = workflow.refreshVolume('volume', 'volume', 0);
        requests.objects.reject(new Error('Read failed'));
        await expect(loading).rejects.toThrow('Read failed');
        expect(workflow.programs).toBe(before);
        expect(workflow.selectedProgramId).toBe('program');
        expect(workflow.activeVolumeId).toBe('volume');
    });

    it('rejects a refresh result superseded by ordinary volume navigation', async () => {
        const old = volumeRequests();
        const next = volumeRequests();
        const { workflow } = workflowHarness(
            new Map([
                ['old', old],
                ['next', next],
            ]),
            new Map([
                [0, old],
                [1, next],
            ]),
        );
        workflow.activeVolumeId = 'old';
        workflow.activePartitionIndex = 0;
        const refreshing = workflow.refreshVolume('old', 'old', 0);
        const navigated = workflow.loadVolume('next', 1);
        resolveVolumeData(next, programObject('new', '001'), '001: New');
        next.contexts.resolve(context(1));
        await navigated;
        workflow.selectedProgramId = 'new';
        resolveVolumeData(old, programObject('old', '001'), '001: Old');
        old.contexts.resolve(context(0));
        await expect(refreshing).rejects.toThrow('workspace changed');
        expect(workflow.activeVolumeId).toBe('next');
        expect(workflow.selectedProgramId).toBe('new');
        expect(workflow.programs[0]?.name).toBe('New');
    });
    it('does not expose a new partition context with Programs retained from the previous volume', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['volume-new', requests]]), new Map([[1, requests]]));
        workflow.programs = [{ objectId: 'program-old', name: 'Old' } as Program];
        workflow.systemProgramContexts = context(0);

        const loading = workflow.loadVolume('volume-new', 1);
        requests.contexts.resolve(context(1));
        await flushPromises();

        expect(workflow.programs).toEqual([]);
        expect(workflow.systemProgramContexts).toBeNull();
        expect(workflow.systemProgramContextsLoading).toBe(true);

        resolveVolumeData(requests, programObject('program-new', '001'), '001: New');
        await loading;

        expect(workflow.programs.map((program) => program.name)).toEqual(['New']);
        expect(workflow.systemProgramContexts?.partitionIndex).toBe(1);
        expect(workflow.systemProgramContextsLoading).toBe(false);
    });

    it('does not commit volume data before its partition context finishes loading', async () => {
        const requests = volumeRequests();
        const { workflow } = workflowHarness(new Map([['volume-new', requests]]), new Map([[1, requests]]));

        const loading = workflow.loadVolume('volume-new', 1);
        resolveVolumeData(requests, programObject('program-new', '001'), '001: New');
        await flushPromises();

        expect(workflow.programs).toEqual([]);
        expect(workflow.objectCount).toBe(0);

        requests.contexts.resolve(context(1));
        await loading;

        expect(workflow.programs.map((program) => program.name)).toEqual(['New']);
        expect(workflow.systemProgramContexts?.partitionIndex).toBe(1);
    });

    it('clears the complete staged snapshot when core volume loading fails', async () => {
        const requests = volumeRequests();
        const { workflow, statuses } = workflowHarness(
            new Map([['volume-broken', requests]]),
            new Map([[1, requests]]),
        );
        workflow.programs = [{ objectId: 'program-old', name: 'Old' } as Program];
        workflow.systemProgramContexts = context(0);

        const loading = workflow.loadVolume('volume-broken', 1);
        requests.contexts.resolve(context(1));
        requests.objects.reject(new Error('Could not load objects'));
        requests.relationships.resolve({ relationships: [], totalCount: 0 });
        requests.names.resolve({ items: [], totalCount: 0 });
        await loading;

        expect(workflow.programs).toEqual([]);
        expect(workflow.relationships).toEqual([]);
        expect(workflow.objectsById.size).toBe(0);
        expect(workflow.objectCount).toBe(0);
        expect(workflow.activeVolumeId).toBe('');
        expect(workflow.activePartitionIndex).toBeNull();
        expect(workflow.systemProgramContexts).toBeNull();
        expect(workflow.systemProgramContextsLoading).toBe(false);
        expect(statuses.at(-1)).toBe('Could not load objects');
    });

    it('keeps a loaded volume usable when only its partition context request fails', async () => {
        const requests = volumeRequests();
        const { workflow, statuses } = workflowHarness(new Map([['volume-new', requests]]), new Map([[1, requests]]));

        const loading = workflow.loadVolume('volume-new', 1);
        resolveVolumeData(requests, programObject('program-new', '001'), '001: New');
        requests.contexts.reject(new Error('System Files unavailable'));
        await loading;

        expect(workflow.programs.map((program) => program.name)).toEqual(['New']);
        expect(workflow.activeVolumeId).toBe('volume-new');
        expect(workflow.activePartitionIndex).toBe(1);
        expect(workflow.systemProgramContexts).toBeNull();
        expect(workflow.systemProgramContextsError).toBe("Could not read the partition's saved System Files.");
        expect(workflow.systemProgramContextsLoading).toBe(false);
        expect(statuses.at(-1)).toBe('Ready');
    });

    it('does not let a superseded load replace the current volume snapshot', async () => {
        const oldRequests = volumeRequests();
        const newRequests = volumeRequests();
        const { workflow } = workflowHarness(
            new Map([
                ['volume-old', oldRequests],
                ['volume-new', newRequests],
            ]),
            new Map([
                [0, oldRequests],
                [1, newRequests],
            ]),
        );

        const oldLoading = workflow.loadVolume('volume-old', 0);
        const newLoading = workflow.loadVolume('volume-new', 1);
        resolveVolumeData(newRequests, programObject('program-new', '001'), '001: New');
        newRequests.contexts.resolve(context(1));
        await newLoading;

        resolveVolumeData(oldRequests, programObject('program-old', '001'), '001: Old');
        oldRequests.contexts.resolve(context(0));
        await oldLoading;

        expect(workflow.activeVolumeId).toBe('volume-new');
        expect(workflow.programs.map((program) => program.name)).toEqual(['New']);
        expect(workflow.systemProgramContexts?.partitionIndex).toBe(1);
    });
});
