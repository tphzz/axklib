import { describe, expect, it, vi } from 'vitest';
import { CapacityWriteRejected, HttpCapacityGate, type CapacityReviewHandler } from './httpCapacityGate';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import { fitsCapacity } from '../test/samplerCapacityFixture';
import { volumeMutationOperation } from './httpTransportWire';
import { MutationCapacityWorkflow } from '../features/mutation/capacityWorkflow.svelte';
import { ASeriesPreferences } from './aSeriesPreferences.svelte';
import { volumeCapacityFixture } from '../test/volumeCapacityFixture';

function setup(reviewer?: ConstructorParameters<typeof HttpCapacityGate>[2]) {
    const invoke = vi.fn().mockResolvedValue({ capacity: fitsCapacity });
    const jobs = { isJob: vi.fn((value) => Boolean(value?.jobId)), map: vi.fn((value) => value) };
    const gate = new HttpCapacityGate(
        { invoke } as unknown as AxklibHttpApiClient,
        jobs as unknown as HttpJobController,
        reviewer,
    );
    return { gate, invoke };
}

describe('HTTP capacity mutation gate', () => {
    it('passes empty-volume context from the frozen canonical manifest, not later caller edits', async () => {
        const operation = volumeMutationOperation({ kind: 'add', partitionIndex: 0, volumeName: 'bar' });
        const request = { manifest: { inline: { operations: [operation] } }, inputBindings: [] };
        const reviewer = vi.fn(async (check, context) => {
            (operation.volume as { samples: unknown[] }).samples.push({ name: 'later edit' });
            expect(context).toEqual({ emptyVolumeCreation: true });
            await check({ target: 'A3000' });
            return { target: 'A3000' as const };
        });
        const { gate } = setup(reviewer);
        await gate.admit(request);
    });

    it.each([
        [{ type: 'rename_volume', volume: { samples: [], waveforms: [] } }],
        [{ type: 'insert_volume', volume: { samples: [{}], waveforms: [] } }],
        [{ type: 'insert_volume', volume: { samples: [], waveforms: [], programs: [{}] } }],
        [{ type: 'insert_volume', volume: { samples: [], waveforms: [], sample_banks: [{}] } }],
        [{ type: 'insert_volume', volume: { samples: [] } }],
        [{ type: 'insert_volume', volume: { samples: [], waveforms: [] } }, { type: 'delete_volume' }],
        [],
    ])(
        'does not treat nonempty, mixed or malformed manifests as automatic empty creation (%j)',
        async (...operations) => {
            const reviewer = vi.fn<CapacityReviewHandler>(async () => ({ target: 'A3000' as const }));
            const { gate } = setup(reviewer);
            await gate.admit({ manifest: { inline: { operations } } });
            expect(reviewer.mock.calls[0][1]).toEqual({ emptyVolumeCreation: false });
        },
    );

    it.each(['A3000', 'A4000_A5000'] as const)(
        'validates and writes an empty bar once for %s without a review dialog',
        async (target) => {
            const review = new MutationCapacityWorkflow(
                new ASeriesPreferences({ load: async () => target, save: async () => {} }),
            );
            const { gate, invoke } = setup((check, context) => review.review(check, context));
            invoke
                .mockResolvedValueOnce({
                    capacity: {
                        ...fitsCapacity,
                        target,
                        reports: [{ ...volumeCapacityFixture(), volumeName: 'bar', objectCounts: [] }],
                    },
                })
                .mockResolvedValueOnce({ jobId: 'j' });
            await gate.start({
                imageId: 'i',
                expectedRevision: 3,
                manifest: {
                    inline: {
                        operations: [volumeMutationOperation({ kind: 'add', partitionIndex: 0, volumeName: 'bar' })],
                    },
                },
                inputBindings: [],
            });
            expect(review.request).toBeNull();
            expect(invoke).toHaveBeenCalledTimes(2);
            expect(invoke.mock.calls.map(([operation]) => operation)).toEqual(['images.alter.inspect', 'images.alter']);
            expect(invoke.mock.calls[1][1].capacityPolicy).toEqual({ target });
            expect(invoke.mock.calls[1][1].expectedRevision).toBe(3);
        },
    );

    it('freezes the request before review and sends only the approved policy', async () => {
        const request = { imageId: 'i', manifest: { value: 'original' } };
        const { gate, invoke } = setup(async (check) => {
            request.manifest.value = 'changed';
            await check({ target: 'A3000' });
            return { target: 'A3000' };
        });
        invoke.mockResolvedValueOnce({ capacity: fitsCapacity }).mockResolvedValueOnce({ jobId: 'j' });
        await gate.start(request);
        expect(invoke.mock.calls[0][1].manifest.value).toBe('original');
        expect(invoke.mock.calls[1][1]).toEqual({
            imageId: 'i',
            manifest: { value: 'original' },
            capacityPolicy: { target: 'A3000' },
        });
    });

    it('never submits a write after cancellation or technical inspection failure', async () => {
        for (const message of ['cancelled', 'Disconnected']) {
            const { gate, invoke } = setup(async () => {
                throw new Error(message);
            });
            await expect(gate.start({ imageId: 'i' })).rejects.toBeInstanceOf(CapacityWriteRejected);
            expect(invoke).not.toHaveBeenCalled();
        }
    });

    it('uses the filesystem inspection and does not relabel post-submission failures', async () => {
        const { gate, invoke } = setup(async (check) => {
            await check({ target: 'A3000' });
            return { target: 'A3000' };
        });
        const network = new Error('lost after POST');
        invoke.mockResolvedValueOnce({ capacity: fitsCapacity }).mockRejectedValueOnce(network);
        await expect(gate.start({ edits: [] }, 'images.filesystem.edit', 'images.filesystem.inspect')).rejects.toBe(
            network,
        );
        expect(invoke.mock.calls[0][0]).toBe('images.filesystem.inspect');
        expect(invoke.mock.calls[1][0]).toBe('images.filesystem.edit');
    });
});
