import { describe, expect, it, vi } from 'vitest';
import { fixedVirtualWindow, virtualViewport } from './virtualList';

describe('fixedVirtualWindow', () => {
    it('bounds the initial render before the viewport has been measured', () => {
        expect(fixedVirtualWindow(200, { scrollTop: 0, height: 0 }, 26)).toEqual({
            startIndex: 0,
            endIndex: 36,
            offset: 0,
            totalHeight: 5_200,
        });
    });

    it('overscans around the visible rows and clamps the final window', () => {
        expect(fixedVirtualWindow(200, { scrollTop: 2_600, height: 260 }, 26)).toEqual({
            startIndex: 94,
            endIndex: 116,
            offset: 2_444,
            totalHeight: 5_200,
        });
        expect(fixedVirtualWindow(200, { scrollTop: 5_200, height: 260 }, 26)).toEqual({
            startIndex: 178,
            endIndex: 200,
            offset: 4_628,
            totalHeight: 5_200,
        });
    });

    it('publishes scroll changes and stops publishing after teardown', () => {
        const node = document.createElement('div');
        Object.defineProperty(node, 'clientHeight', { configurable: true, value: 260 });
        const update = vi.fn();
        const action = virtualViewport(node, update);

        expect(update).toHaveBeenLastCalledWith({ scrollTop: 0, height: 260 });
        node.scrollTop = 2_600;
        node.dispatchEvent(new Event('scroll'));
        expect(update).toHaveBeenLastCalledWith({ scrollTop: 2_600, height: 260 });

        action.destroy();
        node.scrollTop = 3_000;
        node.dispatchEvent(new Event('scroll'));
        expect(update).toHaveBeenCalledTimes(2);
    });
});
