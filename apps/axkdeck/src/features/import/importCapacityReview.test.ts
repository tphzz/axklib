import { describe, expect, it, vi } from 'vitest';
import { ImportCapacityReview } from './importCapacityReview.svelte';
import { fitsCapacity } from '../../test/samplerCapacityFixture';

describe('sampler capacity inspection', () => {
    it('uses only the selected target in the canonical policy', () => {
        expect(new ImportCapacityReview().policy()).toEqual({ target: 'A3000' });
    });

    it('automatically admits a fitting check', async () => {
        const review = new ImportCapacityReview();
        const check = vi.fn().mockResolvedValue(fitsCapacity);
        expect(await review.review(check)).toEqual({ target: 'A3000' });
        expect(check).toHaveBeenCalledWith({ target: 'A3000' });
        expect(review.busy).toBe(false);
    });

    it('blocks a capacity failure without an override path', async () => {
        const review = new ImportCapacityReview();
        await expect(review.review(async () => ({ ...fitsCapacity, allowed: false }))).rejects.toThrow('does not fit');
        expect(review.admission?.allowed).toBe(false);
        expect(review.busy).toBe(false);
    });

    it('retains a technical failure as an ordinary error', async () => {
        const review = new ImportCapacityReview();
        await expect(
            review.review(async () => {
                throw new Error('Disconnected');
            }),
        ).rejects.toThrow('Disconnected');
        expect(review.message).toBe('Disconnected');
        expect(review.admission).toBeNull();
        expect(review.busy).toBe(false);
    });

    it('rejects a response for another target', async () => {
        const review = new ImportCapacityReview();
        await expect(review.review(async () => ({ ...fitsCapacity, target: 'A4000_A5000' }))).rejects.toThrow(
            'target changed',
        );
    });

    it('rejects a target changed during inspection', async () => {
        const review = new ImportCapacityReview();
        const pending = review.review(async () => {
            review.target = 'A4000_A5000';
            return fitsCapacity;
        });
        await expect(pending).rejects.toThrow('target changed');
        expect(review.admission).toBeNull();
    });
});
