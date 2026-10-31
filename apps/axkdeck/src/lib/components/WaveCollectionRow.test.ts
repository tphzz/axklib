import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import WaveCollectionRow from './WaveCollectionRow.svelte';

const props = {
    objectId: 'wave',
    name: 'Wave',
    index: 0,
    metadata: '',
    format: '',
    values: [],
    timeline: {
        sampleRate: 1000,
        storedFrameCount: 1000,
        playbackStartFrame: 0,
        playbackLengthFrames: 1000,
        loopStartFrame: 0,
        loopLengthFrames: 0,
        displayDurationSeconds: 1,
    },
    onselect: vi.fn(),
    onseek: vi.fn(),
    onplay: vi.fn(),
    onstop: vi.fn(),
    onpreview: vi.fn(),
    onkeydown: vi.fn(),
};

describe('wave plot interaction', () => {
    it('selects without playback while idle and exposes actual seeking only during playback', async () => {
        const view = render(WaveCollectionRow, props);
        expect(screen.queryByRole('slider')).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Select Wave waveform' }));
        expect(props.onselect).toHaveBeenCalledOnce();
        expect(props.onplay).not.toHaveBeenCalled();
        expect(props.onseek).not.toHaveBeenCalled();
        await view.rerender({ playing: true, playheadFrame: 250 });
        const seek = screen.getByRole('slider', { name: 'Seek Wave' }) as HTMLInputElement;
        await waitFor(() => expect(seek.valueAsNumber).toBe(0.25));
        expect(seek.getAttribute('aria-valuetext')).toBe('0.250 s');
        await fireEvent.input(seek, { target: { value: '.5' } });
        expect(props.onseek).toHaveBeenCalledWith(0.5);
        expect(props.onplay).not.toHaveBeenCalled();
        seek.focus();
        await view.rerender({ playing: false });
        await waitFor(() =>
            expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Select Wave waveform' })),
        );
        expect(screen.queryByRole('slider')).toBeNull();
    });

    it('retains explicit unavailable reasons and prevents interaction', () => {
        render(WaveCollectionRow, { ...props, unavailable: 'Companion media missing' });
        const select = screen.getByRole('button', { name: 'Select Wave waveform' }) as HTMLButtonElement;
        expect(select.disabled).toBe(true);
        expect(select.title).toBe('Companion media missing');
        expect((screen.getByRole('button', { name: 'Play Wave' }) as HTMLButtonElement).disabled).toBe(true);
    });
});
