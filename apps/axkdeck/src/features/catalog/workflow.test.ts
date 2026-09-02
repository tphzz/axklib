import { describe, expect, it, vi } from 'vitest';
import type {
    ContentPage,
    ImageTransport,
    ObjectPage,
    RelationshipPage,
    SamplerObject,
    SystemProgramContexts,
} from '../../lib/transport';
import type { Program } from '../../lib/types';
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
