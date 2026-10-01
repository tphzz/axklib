import { describe, expect, it, vi } from 'vitest';
import { VolumeCapacityController } from './volumeCapacity.svelte';
import type { VolumeCapacityInspection } from './transport';

const selectedSource = { id: 'v', kind: 'volume' as const, name: 'Test', children: [], childCount: 0 };

function response(revision: number, scope = 'v'): VolumeCapacityInspection {
    return {
        imageId: 'remote',
        revision,
        contentScopeId: scope,
        report: {
            partitionIndex: 0,
            volumeDirectoryId: 9,
            volumeName: 'Test',
            baseline: 'FRESH_POWER_ON_WIPE_VOLUME_LOAD',
            objectCounts: [],
            profiles: [],
        },
    };
}

describe('volume capacity cache', () => {
    it.each(['resolve', 'reject'])(
        'ignores a late %s from deleted bar after recreating the same scope',
        async (outcome) => {
            let revision = 1;
            let resolve!: (value: VolumeCapacityInspection) => void;
            let reject!: (error: Error) => void;
            const inspectVolumeCapacity = vi
                .fn()
                .mockImplementationOnce(
                    () =>
                        new Promise((done, fail) => {
                            resolve = done;
                            reject = fail;
                        }),
                )
                .mockResolvedValue(response(3, 'bar'));
            const controller = new VolumeCapacityController(() => ({
                sessionId: 1,
                revision,
                enabled: true,
                selectedSource,
                transport: { inspectVolumeCapacity },
            }));
            const stale = controller.inspect('bar');
            await Promise.resolve();
            revision = 3;
            await controller.inspect('bar');
            if (outcome === 'resolve') resolve(response(1, 'bar'));
            else reject(new Error('Deleted volume not found'));
            await stale;
            expect(controller.state('bar')).toEqual({ status: 'ready', inspection: response(3, 'bar') });
            expect(inspectVolumeCapacity).toHaveBeenCalledTimes(2);
        },
    );
    it('does not carry a volume inspector selection into another image', async () => {
        let sessionId = 1;
        const controller = new VolumeCapacityController(() => ({
            sessionId,
            revision: 1,
            enabled: true,
            selectedSource,
            transport: { inspectVolumeCapacity: vi.fn().mockResolvedValue(response(1)) },
        }));
        controller.selectVolume({ id: 'v', kind: 'volume', name: 'Test', children: [], childCount: 0 });
        expect(controller.selectedVolume?.id).toBe('v');
        sessionId = 2;
        expect(controller.selectedVolume).toBeNull();
        await Promise.resolve();
    });
    it('deduplicates tooltip and inspector requests by session, revision and scope', async () => {
        const inspectVolumeCapacity = vi.fn().mockResolvedValue(response(2));
        const controller = new VolumeCapacityController(() => ({
            sessionId: 1,
            revision: 2,
            enabled: true,
            selectedSource,
            transport: { inspectVolumeCapacity },
        }));
        await Promise.all([controller.inspect('v'), controller.inspect('v')]);
        expect(inspectVolumeCapacity).toHaveBeenCalledTimes(1);
        expect(controller.state('v')?.status).toBe('ready');
        await controller.inspect('v');
        expect(inspectVolumeCapacity).toHaveBeenCalledTimes(1);
    });

    it('does not publish a response after the image revision changes', async () => {
        let revision = 2;
        let complete!: (result: VolumeCapacityInspection) => void;
        const inspectVolumeCapacity = vi.fn().mockImplementation(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        const controller = new VolumeCapacityController(() => ({
            sessionId: 1,
            revision,
            enabled: true,
            selectedSource,
            transport: { inspectVolumeCapacity },
        }));
        const pending = controller.inspect('v');
        await Promise.resolve();
        revision = 3;
        complete(response(2));
        await pending;
        expect(controller.state('v')).toBeUndefined();
    });

    it('leaves non-A-series volumes alone and exposes inspection errors without a fit claim', async () => {
        let enabled = false;
        const inspectVolumeCapacity = vi.fn().mockRejectedValue(new Error('Disconnected'));
        const controller = new VolumeCapacityController(() => ({
            sessionId: 1,
            revision: 1,
            enabled,
            selectedSource,
            transport: { inspectVolumeCapacity },
        }));
        await controller.inspect('v');
        expect(inspectVolumeCapacity).not.toHaveBeenCalled();
        enabled = true;
        await controller.inspect('v');
        expect(controller.state('v')).toEqual({ status: 'error', message: 'Disconnected' });
    });
});
