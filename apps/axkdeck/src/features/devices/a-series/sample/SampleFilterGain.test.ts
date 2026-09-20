import { fireEvent, render } from '@testing-library/svelte';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { flushSync } from 'svelte';
import { describe, expect, it, vi } from 'vitest';
import SampleFilterGraph from './SampleFilterGraph.svelte';
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

function setup(blockedParameters: string[] = [], disabled = false) {
    const draft = new EditorDraft({
        filter_type: 6,
        filter_cutoff: 44,
        filter_q_width: 4,
        filter_gain: 8,
        filter_cutoff_distance: 20,
    });
    const document = {
        draft,
        detail: { editing: { ...sampleFormatFixture(), blockedParameters, unavailableParameters: {} } },
    } as unknown as ObjectEditorDocument;
    const view = render(SampleFilterGraph, { document, disabled });
    const handle = view.container.querySelector<HTMLButtonElement>('[data-handle="gain"]');
    expect(handle).not.toBeNull();
    return { draft, view, handle: handle! };
}

describe('Filter response gain control', () => {
    it('keeps the transient gain handle reachable by keyboard', async () => {
        const { draft, handle } = setup();
        expect(handle.tabIndex).toBeGreaterThanOrEqual(0);
        expect(handle.hidden).toBe(false);
        expect(handle.disabled).toBe(false);
        await fireEvent.focus(handle);
        expect(handle.classList.contains('trace-visible')).toBe(true);
        expect(draft.dirty).toBe(false);
        await fireEvent.blur(handle);
        expect(handle.classList.contains('trace-visible')).toBe(false);
    });

    it('edits only gain with Up and Down and preserves gesture undo', async () => {
        const { draft, handle } = setup();
        await fireEvent.keyDown(handle, { key: 'ArrowUp' });
        await fireEvent.keyUp(handle, { key: 'ArrowUp' });
        expect(draft.changes).toEqual({ filter_gain: 9 });
        await fireEvent.keyDown(handle, { key: 'ArrowDown' });
        await fireEvent.keyUp(handle, { key: 'ArrowDown' });
        expect(draft.dirty).toBe(false);
        flushSync(() => draft.undo());
        expect(draft.changes).toEqual({ filter_gain: 9 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
    });

    it('uses Home and End for the native gain limits without changing filter shape parameters', async () => {
        const { draft, handle } = setup();
        await fireEvent.keyDown(handle, { key: 'Home' });
        await fireEvent.keyUp(handle, { key: 'Home' });
        expect(draft.changes).toEqual({ filter_gain: -31 });
        await fireEvent.keyDown(handle, { key: 'End' });
        await fireEvent.keyUp(handle, { key: 'End' });
        expect(draft.changes).toEqual({ filter_gain: 31 });
    });

    it('never routes gain wheel or Alt gestures into cutoff Q / Width', async () => {
        const { draft, handle } = setup();
        await fireEvent.wheel(handle, { deltaY: -100 });
        await fireEvent.wheel(handle, { deltaY: -100, shiftKey: true });
        await fireEvent.pointerDown(handle, { altKey: true });
        flushSync(() => gesture.move(0.6, 0.6));
        flushSync(gesture.end);
        await fireEvent.keyDown(handle, { key: 'ArrowUp', altKey: true });
        await fireEvent.keyUp(handle, { key: 'ArrowUp', altKey: true });
        expect(draft.values.filter_q_width).toBe(4);
        expect(draft.values.filter_cutoff).toBe(44);
        expect(draft.values.filter_cutoff_distance).toBe(20);
        expect(Object.keys(draft.changes).every((key) => key === 'filter_gain')).toBe(true);
    });

    it.each([
        { blocked: ['filter_gain'], disabled: false },
        { blocked: [], disabled: true },
    ])('does not edit gain when blocked or the editor is locked: %j', async ({ blocked, disabled }) => {
        const { draft, handle } = setup(blocked, disabled);
        expect(handle.disabled).toBe(true);
        await fireEvent.keyDown(handle, { key: 'ArrowUp' });
        await fireEvent.keyUp(handle, { key: 'ArrowUp' });
        await fireEvent.wheel(handle, { deltaY: -100 });
        expect(draft.dirty).toBe(false);
    });
});
