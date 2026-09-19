import { afterEach, describe, expect, it, vi } from 'vitest';
import { graphDrag } from './graphDrag';

describe('graph drag coordinates', () => {
    afterEach(() => vi.unstubAllGlobals());
    it.each([false, true])('allows time-axis extension only when explicitly enabled: %s', (unboundedX) => {
        const target = document.createElement('button');
        target.setPointerCapture = vi.fn();
        target.hasPointerCapture = () => false;
        vi.stubGlobal(
            'requestAnimationFrame',
            vi.fn(() => 1),
        );
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        const change = vi.fn();
        graphDrag(
            { currentTarget: target, pointerId: 1, clientX: 0, clientY: 0 } as unknown as PointerEvent,
            { x: 1, y: 1 },
            { width: 100, height: 100, unboundedX },
            change,
            vi.fn(),
        );
        const end = new Event('pointerup');
        Object.assign(end, { pointerId: 1, clientX: 40, clientY: -40 });
        target.dispatchEvent(end);
        expect(change).toHaveBeenLastCalledWith(unboundedX ? 1.4 : 1, 1);
    });
    it('preserves the grab offset, uses frozen bounds and includes the release position once', () => {
        let frame: FrameRequestCallback | undefined;
        vi.stubGlobal(
            'requestAnimationFrame',
            vi.fn((callback) => {
                frame = callback;
                return 1;
            }),
        );
        vi.stubGlobal(
            'cancelAnimationFrame',
            vi.fn(() => {
                frame = undefined;
            }),
        );
        const target = document.createElement('button');
        target.setPointerCapture = vi.fn();
        target.releasePointerCapture = vi.fn();
        target.hasPointerCapture = () => true;
        const start = { currentTarget: target, pointerId: 2, clientX: 107, clientY: 53 } as unknown as PointerEvent;
        const change = vi.fn(),
            end = vi.fn();
        const cancel = graphDrag(start, { x: 0.5, y: 0.5 }, { width: 200, height: 100 }, change, end);
        const pointer = (type: string, x: number, y: number, id = 2) => {
            const event = new Event(type);
            Object.assign(event, { pointerId: id, clientX: x, clientY: y });
            target.dispatchEvent(event);
        };
        pointer('pointermove', 127, 43, 3);
        expect(change).not.toHaveBeenCalled();
        pointer('pointermove', 127, 43);
        pointer('pointermove', 129, 42);
        pointer('pointermove', 127, 43);
        expect(change).not.toHaveBeenCalled();
        frame?.(0);
        expect(change).toHaveBeenLastCalledWith(0.6, 0.6);
        pointer('pointerup', 147, 33);
        expect(change).toHaveBeenLastCalledWith(0.7, 0.7);
        pointer('lostpointercapture', 147, 33);
        cancel();
        expect(end).toHaveBeenCalledOnce();
        pointer('pointermove', 180, 33);
        expect(change).toHaveBeenCalledTimes(2);
        expect(frame).toBeUndefined();
    });
    it('cancels pending work on teardown without changing the draft', () => {
        const target = document.createElement('button');
        target.setPointerCapture = vi.fn();
        target.hasPointerCapture = () => false;
        vi.stubGlobal(
            'requestAnimationFrame',
            vi.fn(() => 2),
        );
        const cancelFrame = vi.fn();
        vi.stubGlobal('cancelAnimationFrame', cancelFrame);
        const change = vi.fn();
        const stop = graphDrag(
            { currentTarget: target, pointerId: 1, clientX: 0, clientY: 0 } as unknown as PointerEvent,
            { x: 0.5, y: 0.5 },
            { width: 100, height: 100 },
            change,
            vi.fn(),
        );
        const move = new Event('pointermove');
        Object.assign(move, { pointerId: 1, clientX: 20, clientY: 20 });
        target.dispatchEvent(move);
        stop();
        expect(cancelFrame).toHaveBeenCalledWith(2);
        expect(change).not.toHaveBeenCalled();
    });
});
