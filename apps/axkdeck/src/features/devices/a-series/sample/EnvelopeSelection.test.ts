import { fireEvent, render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import EnvelopeGraph from './EnvelopeGraph.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import { BankDraft } from '../bank/draft.svelte';
import type { SampleEnvelope } from './envelope';

beforeEach(() => {
    Element.prototype.scrollIntoView = vi.fn();
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    );
});

function values(kind: SampleEnvelope) {
    return {
        [`${kind}.attack_rate`]: 127,
        [`${kind}.decay_rate`]: 32,
        [`${kind}.release_rate`]: 44,
        [`${kind}.init_level`]: 0,
        [`${kind}.attack_level`]: 0,
        [`${kind}.sustain_level`]: 0,
        [`${kind}.release_level`]: 0,
    };
}
function setup(kind: SampleEnvelope, draft = new EditorDraft(values(kind)), blocked: string[] = []) {
    const onselect = vi.fn();
    const view = render(EnvelopeGraph, { draft, kind, disabled: false, blocked, onselect });
    const selector = () => view.getByRole('button', { name: 'Envelope stage' }) as HTMLButtonElement;
    const handle = (id: string) => view.container.querySelector<HTMLButtonElement>(`[data-handle="${id}"]`)!;
    async function select(id: string) {
        await fireEvent.click(selector());
        const label = handle(id).getAttribute('aria-label')!.split(':')[0]!;
        await fireEvent.click(view.getByRole('option', { name: label }));
    }
    return { view, draft, selector, select, handle, onselect };
}

describe('envelope stage selection', () => {
    it.each(['feg', 'peg'] as const)('%s selects equal-level points without displacing them', async (kind) => {
        const { view, draft, selector, select, handle, onselect } = setup(kind);
        const points = ['0', '1'].map((id) => [handle(id).style.left, handle(id).style.top]);
        expect(points[0]![1]).toBe(points[1]![1]);
        expect(points[0]![0]).not.toBe(points[1]![0]);
        const trace = view.container.querySelector('[data-trace="envelope"]')!.getAttribute('d');
        expect(selector().textContent).toContain('Initial');
        for (const id of ['1', '0']) {
            await select(id);
            expect(handle(id).getAttribute('aria-pressed')).toBe('true');
            expect(handle(id).classList.contains('selected')).toBe(true);
            expect(view.container.querySelector('output')!.textContent).toMatch(
                id === '0' ? /Initial: 0/ : /Attack: 0/,
            );
        }
        expect(onselect).toHaveBeenLastCalledWith(`${kind}.init_level`);
        expect(['0', '1'].map((id) => [handle(id).style.left, handle(id).style.top])).toEqual(points);
        expect(view.container.querySelector('[data-trace="envelope"]')!.getAttribute('d')).toBe(trace);
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
    });

    it.each(['aeg', 'feg', 'peg'] as const)('%s synchronizes keyboard focus and stage selection', async (kind) => {
        const { selector, handle, draft } = setup(kind);
        await fireEvent.focus(handle('2'));
        expect(selector().textContent).toContain('Sustain');
        await fireEvent.keyDown(handle('2'), { key: 'ArrowUp' });
        await fireEvent.keyUp(handle('2'), { key: 'ArrowUp' });
        expect(draft.changes).toEqual({ [`${kind}.sustain_level`]: 1 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
    });

    it('shows only stage handles and retains amplitude Hold naming', async () => {
        const { selector, view } = setup('aeg', new EditorDraft({ ...values('aeg'), 'aeg.attack_mode': 1 }));
        expect(selector().textContent).toContain('Hold');
        await fireEvent.click(selector());
        expect(view.getAllByRole('option').map((option) => option.textContent?.trim())).toEqual([
            'Hold',
            'Sustain',
            'Release',
        ]);
    });

    it('disables unavailable stages and a fully read-only selector', async () => {
        const blocked = ['feg.init_level', 'feg.attack_level', 'feg.attack_rate'];
        const { selector, view, draft } = setup('feg', undefined, blocked);
        await fireEvent.click(selector());
        expect(view.getAllByRole('option').map((option) => option.getAttribute('aria-disabled') === 'true')).toEqual([
            true,
            true,
            false,
            false,
        ]);
        expect(selector().textContent).toContain('Sustain');
        await view.rerender({ draft, kind: 'feg', disabled: true, blocked, onselect: vi.fn() });
        expect(selector().disabled).toBe(true);
        expect(selector().textContent).toContain('Sustain');
        expect(view.container.querySelector('output')!.textContent).toMatch(/^Sustain:/);
        expect(draft.canUndo).toBe(false);
    });

    it('reveals a selected stage outside the viewport without changing its value', async () => {
        const { select, handle, view, draft } = setup('feg');
        await fireEvent.click(view.getByRole('button', { name: 'Zoom envelope in' }));
        expect(handle('4').hidden).toBe(true);
        await select('4');
        expect(handle('4').hidden).toBe(false);
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
    });

    it('does not activate bank overrides while selecting or hovering stages', async () => {
        const source = values('feg');
        const draft = new BankDraft(
            source,
            Object.keys(source).map((key, id) => ({
                id,
                keys: [key],
                selectors: [id],
                activeSelectors: [],
            })),
        );
        flushSync(() => {
            draft.member = source;
        });
        const { select, handle, view } = setup('feg', draft);
        for (const id of ['1', '0', '4', '2']) {
            await select(id);
            await fireEvent.pointerEnter(handle(id));
            await fireEvent.pointerLeave(handle(id));
            expect(view.container.querySelector('output')!.textContent).toContain('Preview sample values');
        }
        expect(Object.keys(source).some((key) => draft.isOverridden(key))).toBe(false);
        expect(draft.changes).toEqual({});
        expect(draft.canUndo).toBe(false);
    });
});
