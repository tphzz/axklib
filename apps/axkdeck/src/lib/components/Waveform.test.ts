import { render } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import Waveform from './Waveform.svelte';

describe('Waveform', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('uses one ordinary resize observer without resolution media-query subscriptions', () => {
        const observe = vi.fn();
        const disconnect = vi.fn();
        const matchMedia = vi.fn(() => ({
            addEventListener: vi.fn(),
            removeEventListener: vi.fn(),
        }));
        class TestResizeObserver {
            observe = observe;
            disconnect = disconnect;
        }
        vi.stubGlobal('ResizeObserver', TestResizeObserver);
        vi.stubGlobal('matchMedia', matchMedia);

        const { container, unmount } = render(Waveform, { props: { values: [] } });
        const canvas = container.querySelector('canvas');

        expect(canvas).toBeTruthy();
        expect(observe).toHaveBeenCalledOnce();
        expect(observe).toHaveBeenCalledWith(canvas);
        expect(matchMedia).not.toHaveBeenCalled();

        unmount();
        expect(disconnect).toHaveBeenCalledOnce();
    });

    it('positions the Wave window and loop boundaries against the stored-frame timeline', () => {
        const { container } = render(Waveform, {
            props: {
                values: [],
                sourceFrameCount: 1_000,
                timelineFrameCount: 1_000,
                windowStartFrame: 100,
                windowLengthFrames: 800,
                loopStartFrame: 250,
                loopLengthFrames: 500,
            },
        });
        const frame = container.querySelector('.waveform-frame');
        const waveBoundaries = container.querySelectorAll('.waveform-window-boundary');
        const loopBoundaries = container.querySelectorAll('.waveform-loop-boundary');

        expect(frame?.getAttribute('data-window-start-ratio')).toBe('0.1');
        expect(frame?.getAttribute('data-window-end-ratio')).toBe('0.9');
        expect(waveBoundaries[0]?.getAttribute('style')).toContain('10%');
        expect(waveBoundaries[1]?.getAttribute('style')).toContain('90%');
        expect(loopBoundaries[0]?.getAttribute('style')).toContain('25%');
        expect(loopBoundaries[1]?.getAttribute('style')).toContain('75%');
    });
});
