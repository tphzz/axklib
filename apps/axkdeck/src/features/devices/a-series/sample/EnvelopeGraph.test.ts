import { fireEvent, render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import EnvelopeGraph from './EnvelopeGraph.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import { envelopeDuration, envelopeDurations, type SampleEnvelope } from './envelope';

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
beforeEach(() =>
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    ),
);

function setup(kind: SampleEnvelope, blocked: string[] = [], disabled = false, releaseRate = 80) {
    const draft = new EditorDraft({
        [`${kind}.attack_rate`]: 90,
        [`${kind}.decay_rate`]: 100,
        [`${kind}.release_rate`]: releaseRate,
        [`${kind}.init_level`]: 0,
        [`${kind}.attack_level`]: 100,
        [`${kind}.sustain_level`]: 64,
        [`${kind}.release_level`]: -20,
    });
    const durations = envelopeDurations(kind, draft.values);
    const span = durations.reduce((sum, duration) => sum + duration, 32);
    const noteOff = span - durations[2]!;
    const view = render(EnvelopeGraph, { draft, kind, blocked, disabled, onselect: vi.fn() });
    const endpoint = () => view.container.querySelector<HTMLButtonElement>('[data-handle="4"]')!;
    return { draft, view, endpoint, xForRate: (rate: number) => (noteOff + envelopeDuration(rate)) / span };
}

describe('envelope release endpoint', () => {
    it.each(['aeg', 'feg', 'peg'] as const)('%s edits release at the endpoint with one undo', async (kind) => {
        const { view, endpoint, draft, xForRate } = setup(kind);
        expect(view.container.querySelector('[data-handle="release-rate"]')).toBeNull();
        const handle = endpoint();
        expect(handle).not.toBeNull();
        expect(handle.style.left).toBe('100%');
        await fireEvent.pointerDown(handle);
        flushSync(() => gesture.move(xForRate(70), (25 + 127) / 254));
        flushSync(gesture.end);
        expect(draft.changes).toEqual(
            kind === 'aeg'
                ? { 'aeg.release_rate': 70 }
                : { [`${kind}.release_rate`]: 70, [`${kind}.release_level`]: 25 },
        );
        if (kind === 'aeg') expect(handle.style.top).toBe('100%');
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
    });
    it('allows lengthening an immediate amplitude release beyond the fixed viewport', async () => {
        const { endpoint, draft, xForRate } = setup('aeg', [], false, 127);
        await fireEvent.pointerDown(endpoint());
        flushSync(() => gesture.move(xForRate(100), 0));
        expect(endpoint().hidden).toBe(true);
        flushSync(() => gesture.move(xForRate(60), 0));
        flushSync(gesture.end);
        expect(draft.values['aeg.release_rate']).toBe(60);
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        expect(endpoint().hidden).toBe(false);
    });
    it.each(['rate', 'level'])('respects a blocked release %s without blocking the other axis', async (blocked) => {
        const { endpoint, draft, xForRate } = setup('peg', [`peg.release_${blocked}`]);
        await fireEvent.pointerDown(endpoint());
        flushSync(() => gesture.move(xForRate(70), (25 + 127) / 254));
        flushSync(gesture.end);
        expect(draft.changes).toEqual(blocked === 'rate' ? { 'peg.release_level': 25 } : { 'peg.release_rate': 70 });
    });
    it('uses left/right for rate and up/down for level and preserves bounds', async () => {
        const { endpoint, draft } = setup('feg');
        for (const key of ['ArrowLeft', 'ArrowUp']) {
            await fireEvent.keyDown(endpoint(), { key });
            await fireEvent.keyUp(endpoint(), { key });
        }
        expect(draft.changes).toEqual({ 'feg.release_rate': 81, 'feg.release_level': -19 });
        await fireEvent.keyDown(endpoint(), { key: 'End' });
        await fireEvent.keyUp(endpoint(), { key: 'End' });
        expect(draft.values['feg.release_level']).toBe(127);
    });
    it('keeps disabled endpoint controls read-only', () => {
        expect(setup('aeg', [], true).endpoint().disabled).toBe(true);
    });
});
