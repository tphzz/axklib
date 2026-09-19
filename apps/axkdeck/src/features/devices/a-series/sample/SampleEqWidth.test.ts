import { fireEvent, render, cleanup } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import SampleEqGraph from './SampleEqGraph.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

const gesture = vi.hoisted(() => ({ move: (_x: number, _y: number) => {}, end: () => {} }));
vi.mock('../../../object-editor/graphDrag', () => ({
    graphDrag: (
        _event: PointerEvent,
        _origin: unknown,
        _bounds: unknown,
        change: typeof gesture.move,
        end: () => void,
    ) => {
        gesture.move = change;
        gesture.end = end;
        return end;
    },
}));
beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    );
});
afterEach(() => {
    cleanup();
    vi.useRealTimers();
    vi.unstubAllGlobals();
});
function setup(type = 0, blocked = false, disabled = false) {
    const draft = new EditorDraft({
        sample_eq_type: type,
        sample_eq_frequency: 26,
        sample_eq_gain_db: 4,
        sample_eq_width_tenths: 60,
    });
    const document = {
        draft,
        detail: { editing: { blockedParameters: blocked ? ['sample_eq_width_tenths'] : [], eqCoefficients: [] } },
    } as unknown as ObjectEditorDocument;
    const view = render(SampleEqGraph, { document, disabled });
    const handle = view.getByRole('button', { name: /EQ frequency \/ gain/ });
    return { draft, handle };
}
describe('EQ width gestures', () => {
    it.each([
        [{ deltaY: -100 }, 65],
        [{ deltaY: -100, shiftKey: true }, 61],
        [{ deltaX: 100, shiftKey: true }, 59],
        [{ deltaY: 100 }, 55],
    ])('uses coarse wheel steps and fine Shift steps: %j', async (event, expected) => {
        const { draft, handle } = setup();
        await fireEvent.wheel(handle, event);
        expect(draft.changes).toEqual({ sample_eq_width_tenths: expected });
    });
    it('groups wheel events into one undo and clamps both ends', async () => {
        const { draft, handle } = setup();
        for (let i = 0; i < 20; i++) await fireEvent.wheel(handle, { deltaY: -1 });
        expect(draft.values.sample_eq_width_tenths).toBe(120);
        await vi.advanceTimersByTimeAsync(180);
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
        for (let i = 0; i < 30; i++) await fireEvent.wheel(handle, { deltaY: 1 });
        expect(draft.values.sample_eq_width_tenths).toBe(10);
    });
    it('doubles Alt-drag width sensitivity without moving frequency or gain', async () => {
        const { draft, handle } = setup();
        await fireEvent.pointerDown(handle, { altKey: true });
        flushSync(() => gesture.move(0.6, 0.5));
        flushSync(gesture.end);
        expect(draft.changes).toEqual({ sample_eq_width_tenths: 82 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
    });
    it('uses fine Alt+Shift keyboard steps and keeps Home/End bounds', async () => {
        const { draft, handle } = setup();
        for (const [key, shiftKey, expected] of [
            ['ArrowRight', false, 65],
            ['ArrowLeft', true, 64],
            ['Home', false, 10],
            ['End', true, 120],
        ] as const) {
            await fireEvent.keyDown(handle, { key, altKey: true, shiftKey });
            await fireEvent.keyUp(handle, { key });
            expect(draft.values.sample_eq_width_tenths).toBe(expected);
        }
        expect(draft.values.sample_eq_frequency).toBe(26);
        expect(draft.values.sample_eq_gain_db).toBe(4);
    });
    it.each([
        [1, false, false],
        [2, false, false],
        [0, true, false],
        [0, false, true],
    ] as const)('does not edit unavailable width (%s, %s, %s)', async (type, blocked, disabled) => {
        const { draft, handle } = setup(type, blocked, disabled);
        await fireEvent.wheel(handle, { deltaY: -1 });
        await fireEvent.pointerDown(handle, { altKey: true });
        if (!disabled) {
            flushSync(() => gesture.move(0.6, 0.5));
            flushSync(gesture.end);
        }
        await fireEvent.keyDown(handle, { key: 'ArrowRight', altKey: true });
        await fireEvent.keyUp(handle, { key: 'ArrowRight' });
        expect(draft.dirty).toBe(false);
    });
    it('leaves Ctrl-wheel and ordinary horizontal scrolling alone', async () => {
        const { draft, handle } = setup();
        await fireEvent.wheel(handle, { deltaY: -100, ctrlKey: true });
        await fireEvent.wheel(handle, { deltaX: -100 });
        expect(draft.dirty).toBe(false);
    });
});
