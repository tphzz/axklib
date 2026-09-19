import { afterEach, describe, expect, it, vi } from 'vitest';
import { mountEditorPopup } from './editorPopup';

afterEach(() => {
    document.body.replaceChildren();
    vi.unstubAllGlobals();
});

function setup(top: number, scale = 1) {
    let resize = () => {};
    const disconnect = vi.fn();
    vi.stubGlobal(
        'ResizeObserver',
        class {
            constructor(callback: () => void) {
                resize = callback;
            }
            observe() {}
            disconnect = disconnect;
        },
    );
    const anchor = document.createElement('button');
    const popup = document.createElement('div');
    document.body.append(anchor);
    Object.defineProperty(anchor, 'offsetWidth', { value: 200 });
    const rect = () => ({ top, bottom: top + 26 * scale, left: 100, width: 200 * scale });
    anchor.getBoundingClientRect = () => rect() as DOMRect;
    let height = 260 * scale;
    popup.getBoundingClientRect = () => {
        const measured = Math.min(height, parseFloat(popup.style.maxHeight) * scale || Infinity);
        const top = parseFloat(popup.style.top || '0') * scale;
        return { top, bottom: top + measured, width: 200 * scale, height: measured } as DOMRect;
    };
    const close = vi.fn();
    const mounted = mountEditorPopup(popup, anchor, close);
    return {
        anchor,
        popup,
        close,
        disconnect,
        mounted,
        resize: (value: number) => {
            height = value * scale;
            resize();
        },
    };
}

describe('editor popup anchoring', () => {
    it.each([1, 1.5, 2])('keeps the upward edge anchored while filtering and clearing at scale %s', (scale) => {
        const { popup, mounted, resize, disconnect } = setup(600, scale);
        expect(popup.getBoundingClientRect().bottom).toBeCloseTo(596);
        for (const height of [34, 40, 260, 34]) {
            resize(height);
            expect(popup.getBoundingClientRect().bottom).toBeCloseTo(596);
        }
        mounted.destroy();
        expect(disconnect).toHaveBeenCalledOnce();
        expect(popup.isConnected).toBe(false);
    });
    it('keeps downward placement even if the result list grows beyond the space below', () => {
        const { popup, mounted, resize } = setup(100);
        const top = popup.getBoundingClientRect().top;
        resize(34);
        expect(popup.getBoundingClientRect().top).toBe(top);
        resize(1000);
        expect(popup.getBoundingClientRect().top).toBe(top);
        expect(popup.getBoundingClientRect().bottom).toBeLessThanOrEqual(innerHeight - 8);
        mounted.destroy();
    });
    it('removes observers and dismissal listeners when unmounted', () => {
        const { anchor, popup, mounted, close } = setup(600);
        anchor.dispatchEvent(new Event('pointerdown', { bubbles: true }));
        popup.dispatchEvent(new Event('pointerdown', { bubbles: true }));
        expect(close).not.toHaveBeenCalled();
        window.dispatchEvent(new Event('resize'));
        expect(close).toHaveBeenCalledOnce();
        mounted.destroy();
        window.dispatchEvent(new Event('resize'));
        expect(close).toHaveBeenCalledOnce();
    });
});
