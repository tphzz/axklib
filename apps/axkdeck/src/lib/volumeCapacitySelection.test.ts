import { describe, expect, it, vi } from 'vitest';
import { VolumeCapacityController } from './volumeCapacity.svelte';
import type { DiskTreeItem } from './types';
import { volumeCapacityFixture } from '../test/volumeCapacityFixture';

const volume = (id: string, name = 'foo'): DiskTreeItem => ({
    id,
    name,
    kind: 'volume',
    partitionIndex: 0,
    volumeDirectoryId: 9,
    childCount: 0,
});

function setup() {
    const context = {
        sessionId: 1 as number | null,
        revision: 1,
        enabled: true,
        selectedSource: volume('old'),
        transport: {
            inspectVolumeCapacity: vi.fn(async (_session: number, contentScopeId: string) => ({
                imageId: 'image',
                revision: context.revision,
                contentScopeId,
                report: volumeCapacityFixture(),
            })),
        },
    };
    return { context, capacity: new VolumeCapacityController(() => context) };
}

describe('capacity workspace selection', () => {
    it('uses the refreshed workspace volume instead of the obsolete content ID', () => {
        const { context, capacity } = setup();
        capacity.selectVolume(context.selectedSource);
        context.revision++;
        context.selectedSource = { ...volume('fresh'), sizeBytes: 200000 };
        expect(capacity.selectedVolume).toEqual(context.selectedSource);
    });

    it('follows a new destination and renamed volume without a second tree click', () => {
        const { context, capacity } = setup();
        capacity.selectVolume(context.selectedSource);
        context.revision++;
        context.selectedSource = volume('destination', 'New import');
        expect(capacity.selectedVolume?.id).toBe('destination');
        context.selectedSource = volume('renamed', 'Renamed');
        expect(capacity.selectedVolume?.name).toBe('Renamed');
    });

    it('does not keep volume details when the workspace selects a partition or closes', () => {
        const { context, capacity } = setup();
        capacity.selectVolume(context.selectedSource);
        context.selectedSource = { id: 'partition', name: 'Partition 1', kind: 'partition', childCount: 0 };
        expect(capacity.selectedVolume).toBeNull();
        context.sessionId = null;
        expect(capacity.selectedVolume).toBeNull();
    });

    it('does not reopen volume details after explicit object selection', () => {
        const { context, capacity } = setup();
        capacity.selectVolume(context.selectedSource);
        capacity.showObject();
        context.revision++;
        context.selectedSource = volume('fresh');
        expect(capacity.selectedVolume).toBeNull();
    });

    it('does not carry volume-inspector intent into another image', () => {
        const { context, capacity } = setup();
        capacity.selectVolume(context.selectedSource);
        context.sessionId = 2;
        context.selectedSource = volume('other-image');
        expect(capacity.selectedVolume).toBeNull();
    });

    it.each(['resolve', 'reject'])('ignores a late %s for an obsolete volume ID', async (outcome) => {
        const { context, capacity } = setup();
        let resolve!: (value: Awaited<ReturnType<typeof context.transport.inspectVolumeCapacity>>) => void;
        let reject!: (error: Error) => void;
        context.transport.inspectVolumeCapacity.mockImplementationOnce(
            () =>
                new Promise((yes, no) => {
                    resolve = yes;
                    reject = no;
                }),
        );
        capacity.selectVolume(context.selectedSource);
        const stale = capacity.inspect('old');
        await Promise.resolve();
        context.revision = 2;
        context.selectedSource = volume('fresh');
        await capacity.inspect('fresh');
        if (outcome === 'resolve')
            resolve({ imageId: 'image', revision: 1, contentScopeId: 'old', report: volumeCapacityFixture() });
        else reject(new Error('Choose an A-Series SFS volume for capacity inspection'));
        await stale;
        expect(capacity.selectedVolume?.id).toBe('fresh');
        expect(capacity.state('fresh')?.status).toBe('ready');
        expect(context.transport.inspectVolumeCapacity).toHaveBeenCalledTimes(2);
    });
});
