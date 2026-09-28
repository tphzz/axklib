import { render, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import WaveCanvas from './WaveCanvas.svelte';

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

describe.each([false, true])('WaveCanvas physical pixels with overview=%s', (overview) => {
    it.each([1, 1.25, 1.5, 2])('keeps envelope and PCM rectangles aligned at DPR %s', async (ratio) => {
        const width = 321.25;
        const height = overview ? 40.25 : 160.25;
        const context = { clearRect: vi.fn(), fillRect: vi.fn(), fillStyle: '', globalAlpha: 1 };
        vi.stubGlobal('devicePixelRatio', ratio);
        vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(
            context as unknown as CanvasRenderingContext2D,
        );
        vi.spyOn(Element.prototype, 'getBoundingClientRect').mockReturnValue(new DOMRect(0, 0, width, height));
        const bins = Array.from({ length: 1024 }, () => ({ minimum: -0.25, maximum: 0.25 }));
        const frames = 8192;
        const view = render(WaveCanvas, { bins, frames, overview });
        const canvas = view.container.querySelector('canvas')!;
        await waitFor(() => expect(canvas.width).toBe(Math.round(width * ratio)));
        expect(canvas.height).toBe(Math.round(height * ratio));
        const envelopeCalls = context.fillRect.mock.calls.slice(-(canvas.width + 1));
        expect(envelopeCalls).toHaveLength(canvas.width + 1);
        for (const rectangle of envelopeCalls) {
            expect(rectangle.every(Number.isInteger)).toBe(true);
        }

        context.fillRect.mockClear();
        const pcm = Float32Array.from({ length: frames }, (_, index) => (index % 2 ? 0.25 : -0.25));
        await view.rerender({ pcm });
        await waitFor(() => expect(context.fillRect).toHaveBeenCalledTimes(canvas.width + 1));
        expect(context.fillRect.mock.calls).toEqual(envelopeCalls);
    });
});
