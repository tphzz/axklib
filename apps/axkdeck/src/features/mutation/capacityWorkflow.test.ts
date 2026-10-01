import { describe, expect, it, vi } from 'vitest';
import { MutationCapacityWorkflow } from './capacityWorkflow.svelte';
import { ASeriesPreferences } from '../../lib/aSeriesPreferences.svelte';
import { fitsCapacity } from '../../test/samplerCapacityFixture';
import type { CapacityAdmission } from '../../lib/importCapacity';
import { volumeCapacityFixture } from '../../test/volumeCapacityFixture';

function workflow(generation: 'A3000' | 'A4000_A5000' = 'A4000_A5000') {
    return new MutationCapacityWorkflow(new ASeriesPreferences({ load: async () => generation, save: async () => {} }));
}
const report = volumeCapacityFixture();
const fits: CapacityAdmission = { ...fitsCapacity, target: 'A4000_A5000', reports: [report] };

describe('mutation capacity review', () => {
    it.each(['A3000', 'A4000_A5000'] as const)('automatically validates an empty volume for %s', async (target) => {
        const review = workflow(target);
        const check = vi.fn(async () => ({ ...fits, target, reports: [{ ...report, objectCounts: [] }] }));
        const pending = review.review(check, { emptyVolumeCreation: true });
        try {
            await vi.waitFor(() => expect(check).toHaveBeenCalledOnce());
            expect(review.request).toBeNull();
            expect(await pending).toEqual({ target });
        } finally {
            review.dispose();
            await pending.catch(() => undefined);
        }
    });

    it.each([false, true])(
        'retains review for unexpected populated reports or denied admission (%s)',
        async (populated) => {
            const review = workflow();
            const pending = review.review(
                async () => ({
                    ...fits,
                    allowed: populated,
                    reports: [{ ...report, objectCounts: populated ? report.objectCounts : [] }],
                }),
                { emptyVolumeCreation: true },
            );
            const rejection = expect(pending).rejects.toThrow('cancelled');
            await vi.waitFor(() => expect(review.request).not.toBeNull());
            review.cancel();
            await rejection;
        },
    );

    it('rejects a failed or mismatched empty-volume inspection without opening a review', async () => {
        for (const check of [
            async () => {
                throw new Error('Disconnected');
            },
            async () => ({ ...fits, target: 'A3000' as const, reports: [{ ...report, objectCounts: [] }] }),
        ]) {
            const review = workflow();
            await expect(review.review(check, { emptyVolumeCreation: true })).rejects.toThrow();
            expect(review.request).toBeNull();
        }
    });

    it('uses the preference and skips the dialog for plain filesystem changes', async () => {
        const review = workflow();
        const check = vi.fn(async (policy) => ({ ...fitsCapacity, target: policy.target }));
        expect(await review.review(check)).toEqual({ target: 'A4000_A5000' });
        expect(review.request).toBeNull();
        expect(check).toHaveBeenCalledWith({ target: 'A4000_A5000' });
    });

    it('checks again on Continue and permits changing the preferred target', async () => {
        const review = workflow();
        const check = vi.fn(async (policy) => ({ ...fits, target: policy.target }));
        const pending = review.review(check);
        await vi.waitFor(() => expect(review.request).not.toBeNull());
        review.request!.review.target = 'A3000';
        review.request!.review.reset();
        await review.inspect();
        await review.submit();
        expect(await pending).toEqual({ target: 'A3000' });
        expect(check).toHaveBeenCalledTimes(3);
        expect(review.request).toBeNull();
    });

    it('keeps a capacity failure blocked until a fitting target is selected', async () => {
        const review = workflow();
        const check = vi.fn(async (policy) => ({ ...fits, target: policy.target, allowed: policy.target === 'A3000' }));
        const pending = review.review(check);
        await vi.waitFor(() => expect(review.request).not.toBeNull());
        await review.submit();
        expect(review.request?.review.message).toContain('does not fit');
        review.request!.review.target = 'A3000';
        await review.inspect();
        await review.submit();
        expect(await pending).toEqual({ target: 'A3000' });
    });

    it('cancels without a policy during an inspection', async () => {
        const review = workflow();
        const pending = review.review(async () => fits);
        const rejection = expect(pending).rejects.toThrow('nothing was written');
        await vi.waitFor(() => expect(review.request).not.toBeNull());
        let resolve!: (value: CapacityAdmission) => void;
        review.request!.check = () =>
            new Promise((done) => {
                resolve = done;
            });
        const inspect = review.inspect();
        review.cancel();
        resolve(fits);
        await inspect;
        await rejection;
        expect(review.request).toBeNull();
    });

    it('clears a prior fitting result when inspection fails', async () => {
        const review = workflow();
        const pending = review.review(async () => fits);
        const rejection = expect(pending).rejects.toThrow('cancelled');
        await vi.waitFor(() => expect(review.request).not.toBeNull());
        review.request!.check = async () => {
            throw new Error('Disconnected');
        };
        await review.inspect();
        expect(review.request?.review.message).toBe('Disconnected');
        expect(review.request?.review.admission).toBeNull();
        review.cancel();
        await rejection;
    });

    it('disposes a pending review without a policy', async () => {
        const review = workflow();
        const pending = review.review(async () => fits);
        const rejection = expect(pending).rejects.toThrow('cancelled');
        await vi.waitFor(() => expect(review.request).not.toBeNull());
        review.dispose();
        await rejection;
        expect(review.request).toBeNull();
    });
});
