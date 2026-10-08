import { describe, expect, it } from 'vitest';
import {
    clampViewport,
    fitViewport,
    minimumKeyboardSpan,
    resizeViewport,
    viewportKey,
    zoomViewport,
} from './mappingViewport';

describe('shared mapping viewport', () => {
    it('caps compact white keys at 20 CSS px without restricting full mapping detail', () => {
        expect(minimumKeyboardSpan(128, true)).toBe(13);
        expect(minimumKeyboardSpan(500, true)).toBe(50);
        expect(minimumKeyboardSpan(3440, true)).toBe(128);
        expect(minimumKeyboardSpan(5120, false)).toBe(12);
    });
    it('clamps every navigation operation to MIDI boundaries and the current sizing cap', () => {
        expect(clampViewport({ start: -20, span: 1 }, 50)).toEqual({ start: 0, span: 50 });
        expect(clampViewport({ start: 127, span: 256 }, 12)).toEqual({ start: 0, span: 128 });
        expect(zoomViewport({ start: 64, span: 64 }, 0.5, 50)).toEqual({ start: 71, span: 50 });
        expect(fitViewport(60, 60, 50)).toEqual({ start: 36, span: 50 });
        expect(fitViewport(127, 127, 12)).toEqual({ start: 115, span: 13 });
    });
    it('preserves the visible center when a resize requires a wider span', () => {
        expect(resizeViewport({ start: 30, span: 32 }, 64)).toEqual({ start: 14, span: 64 });
        expect(resizeViewport({ start: 96, span: 32 }, 64)).toEqual({ start: 64, span: 64 });
        expect(resizeViewport({ start: 30, span: 64 }, 12)).toEqual({ start: 30, span: 64 });
    });
    it('supports semitone, octave, page and absolute keyboard navigation', () => {
        const view = { start: 30, span: 32 };
        expect(viewportKey(view, 'ArrowRight')).toEqual({ start: 31, span: 32 });
        expect(viewportKey(view, 'ArrowLeft', true)).toEqual({ start: 18, span: 32 });
        expect(viewportKey(view, 'PageUp')).toEqual({ start: 0, span: 32 });
        expect(viewportKey(view, 'PageDown')).toEqual({ start: 62, span: 32 });
        expect(viewportKey(view, 'Home')).toEqual({ start: 0, span: 32 });
        expect(viewportKey(view, 'End')).toEqual({ start: 96, span: 32 });
        expect(viewportKey(view, 'Enter')).toBeNull();
    });
});
