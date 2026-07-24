import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import AuditionBar from './AuditionBar.svelte';

describe('AuditionBar', () => {
    it('owns autoplay only in audio-capable views without starting playback', async () => {
        const onautoplaychange = vi.fn();
        render(AuditionBar, {
            props: {
                available: true,
                autoplay: false,
                state: { objectId: null, status: 'idle', playheadFrame: 0 },
                label: '',
                onautoplaychange,
            },
        });

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Autoplay' }));
        expect(onautoplaychange).toHaveBeenCalledWith(true);
    });

    it('reports playback status without duplicating the owning row stop control', () => {
        render(AuditionBar, {
            props: {
                available: true,
                autoplay: true,
                state: { objectId: 'sample-1', status: 'playing', playheadFrame: 0 },
                label: 'Bank A · Sample 1',
                onautoplaychange: vi.fn(),
            },
        });

        expect(screen.getByText('Playing Bank A · Sample 1')).toBeTruthy();
        expect(screen.queryByRole('button', { name: 'Stop playback' })).toBeNull();
    });

    it('does not occupy the Programs view while idle', () => {
        render(AuditionBar, {
            props: {
                available: false,
                autoplay: false,
                state: { objectId: null, status: 'idle', playheadFrame: 0 },
                label: '',
                onautoplaychange: vi.fn(),
            },
        });

        expect(screen.queryByRole('region', { name: 'Audition controls' })).toBeNull();
    });
});
