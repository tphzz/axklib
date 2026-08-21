import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { DelayedExportProgressVisibility, EXPORT_PROGRESS_DELAY_MS } from './progressVisibility.svelte';

describe('DelayedExportProgressVisibility', () => {
    beforeEach(() => vi.useFakeTimers());

    afterEach(() => {
        vi.useRealTimers();
    });

    it('keeps fast exports hidden', async () => {
        const visibility = new DelayedExportProgressVisibility();

        visibility.update('package-export');
        await vi.advanceTimersByTimeAsync(EXPORT_PROGRESS_DELAY_MS - 1);
        visibility.update(null);
        await vi.advanceTimersByTimeAsync(1);

        expect(visibility.operation).toBeNull();
    });

    it('reveals an export that remains busy for the complete delay', async () => {
        const visibility = new DelayedExportProgressVisibility();

        visibility.update('audio-export');
        await vi.advanceTimersByTimeAsync(EXPORT_PROGRESS_DELAY_MS);

        expect(visibility.operation).toBe('audio-export');
    });

    it('restarts the delay when a different export supersedes the pending operation', async () => {
        const visibility = new DelayedExportProgressVisibility();

        visibility.update('package-export');
        await vi.advanceTimersByTimeAsync(EXPORT_PROGRESS_DELAY_MS - 1);
        visibility.update('media-export');
        await vi.advanceTimersByTimeAsync(1);

        expect(visibility.operation).toBeNull();

        await vi.advanceTimersByTimeAsync(EXPORT_PROGRESS_DELAY_MS - 1);
        expect(visibility.operation).toBe('media-export');
    });

    it('clears visible progress when the export finishes', async () => {
        const visibility = new DelayedExportProgressVisibility();
        visibility.update('sequence-export');
        await vi.advanceTimersByTimeAsync(EXPORT_PROGRESS_DELAY_MS);

        visibility.update(null);

        expect(visibility.operation).toBeNull();
    });
});
