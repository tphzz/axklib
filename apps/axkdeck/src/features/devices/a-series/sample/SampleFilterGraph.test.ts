import { fireEvent, render } from '@testing-library/svelte';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { flushSync } from 'svelte';
import { describe, expect, it } from 'vitest';
import SampleFilterGraph from './SampleFilterGraph.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

function setup(type = 6, blockedParameters: string[] = []) {
    const draft = new EditorDraft({
        filter_type: type,
        filter_cutoff: 44,
        filter_q_width: 4,
        filter_gain: 8,
        filter_cutoff_distance: 20,
    });
    const document = {
        draft,
        detail: { editing: { ...sampleFormatFixture(), blockedParameters } },
    } as unknown as ObjectEditorDocument;
    const view = render(SampleFilterGraph, { document, disabled: false });
    return { draft, view };
}

describe('curve-anchored Sample filter controls', () => {
    it.each(Array.from({ length: 17 }, (_, type) => type))(
        'anchors type %i cutoffs to the rendered response',
        (type) => {
            const { view } = setup(type);
            const handles = [
                ...view.container.querySelectorAll<HTMLButtonElement>('[data-handle]:not([data-handle="gain"])'),
            ];
            expect(handles).toHaveLength(type === 0 ? 0 : type < 10 ? 1 : 2);
            const points = view.container
                .querySelector('[data-trace="filter"]')!
                .getAttribute('d')!
                .split(/[ML]/)
                .slice(1)
                .map((pair) => pair.split(',').map(Number));
            for (const handle of handles) {
                const x = Number.parseFloat(handle.style.left) * 10;
                const y = Number.parseFloat(handle.style.top) * 2;
                const next = Math.min(points.length - 1, Math.ceil(x / 2.5));
                const [leftX, leftY] = points[Math.max(0, next - 1)]!;
                const [rightX, rightY] = points[next]!;
                const expected =
                    rightX === leftX ? rightY : leftY! + ((rightY! - leftY!) * (x - leftX!)) / (rightX! - leftX!);
                expect(Math.abs(y - expected!)).toBeLessThan(0.15);
                expect(y).toBeGreaterThanOrEqual(0);
                expect(y).toBeLessThanOrEqual(200);
            }
        },
    );

    it('uses coarse and fine Q wheel steps with a single undo and leaves gain and cutoff unchanged', async () => {
        const { draft, view } = setup();
        const handle = view.container.querySelector('[data-handle]')!;
        await fireEvent.wheel(handle, { deltaY: -1 });
        await fireEvent.wheel(handle, { deltaY: -1, shiftKey: true });
        expect(draft.changes).toEqual({ filter_q_width: 8 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
    });

    it('keeps coincident compound cutoffs independently keyboard-editable', async () => {
        const { draft, view } = setup(11);
        flushSync(() => draft.accept({ ...draft.values, filter_cutoff_distance: 0 }));
        const primary = view.container.querySelector<HTMLButtonElement>('[data-handle="cutoff-q"]')!;
        const secondary = view.container.querySelector<HTMLButtonElement>('[data-handle="distance"]')!;
        expect(primary.style.left).toBe(secondary.style.left);
        expect(primary.style.top).toBe(secondary.style.top);
        await fireEvent.focus(secondary);
        await fireEvent.keyDown(secondary, { key: 'ArrowRight' });
        await fireEvent.keyUp(secondary, { key: 'ArrowRight' });
        expect(draft.changes).toEqual({ filter_cutoff_distance: 1 });
        flushSync(() => draft.undo());
        await fireEvent.keyDown(primary, { key: 'End' });
        await fireEvent.keyUp(primary, { key: 'End' });
        expect(draft.changes).toEqual({ filter_cutoff: 127 });
    });

    it('does not intercept blocked Q or browser zoom wheel gestures', () => {
        const { draft, view } = setup(6, ['filter_q_width']);
        const handle = view.container.querySelector('[data-handle]')!;
        for (const ctrlKey of [false, true]) {
            const event = new WheelEvent('wheel', { deltaY: -1, cancelable: true, ctrlKey });
            handle.dispatchEvent(event);
            expect(event.defaultPrevented).toBe(false);
        }
        expect(draft.dirty).toBe(false);
    });
});
