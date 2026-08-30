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
});
