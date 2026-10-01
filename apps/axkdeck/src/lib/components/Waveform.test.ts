import { render } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import Waveform from './Waveform.svelte';

describe('Waveform', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('observes size and monitor resolution changes and releases both subscriptions', () => {
        const observe = vi.fn();
        const disconnect = vi.fn();
        const addEventListener = vi.fn();
        const removeEventListener = vi.fn();
        const matchMedia = vi.fn(() => ({ addEventListener, removeEventListener }));
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
        expect(matchMedia).toHaveBeenCalledWith('(resolution: 1dppx)');
        const changed = addEventListener.mock.calls[0]![1] as () => void;
        vi.stubGlobal('devicePixelRatio', 1.5);
        changed();
        expect(removeEventListener).toHaveBeenCalledWith('change', changed);
        expect(matchMedia).toHaveBeenLastCalledWith('(resolution: 1.5dppx)');

        unmount();
        expect(disconnect).toHaveBeenCalledOnce();
        expect(removeEventListener).toHaveBeenCalledTimes(2);
    });

    it('positions the Wave window and loop boundaries against the stored-frame timeline', () => {
        const { container } = render(Waveform, {
            props: {
                values: [],
                timeline: {
                    sampleRate: 1_000,
                    storedFrameCount: 1_000,
                    playbackStartFrame: 100,
                    playbackLengthFrames: 800,
                    loopStartFrame: 250,
                    loopLengthFrames: 500,
                    displayDurationSeconds: 1,
                },
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
