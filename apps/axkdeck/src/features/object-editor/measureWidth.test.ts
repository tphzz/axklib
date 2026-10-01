import { describe, expect, it, vi } from 'vitest';
import { measureWidth } from './measureWidth';

describe('editor measured layout', () => {
    it('uses CSS layout pixels, not zoomed screen pixels, and disconnects', () => {
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
        const node = document.createElement('div');
        let width = 803;
        Object.defineProperty(node, 'clientWidth', { get: () => width });
        node.getBoundingClientRect = () => ({ width: 1204.5 }) as DOMRect;
        const change = vi.fn();
        const action = measureWidth(node, { scope: 'editor', change });
        expect(node.dataset.editorUnder?.split(' ')).toContain('900');
        expect(change).toHaveBeenLastCalledWith(803);
        resize();
        expect(change).toHaveBeenCalledTimes(1);
        width = 920;
        resize();
        expect(node.dataset.editorUnder?.split(' ')).not.toContain('900');
        expect(change).toHaveBeenLastCalledWith(920);
        action.destroy();
        expect(disconnect).toHaveBeenCalledOnce();
        vi.unstubAllGlobals();
    });
});
