import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { fireEvent, render } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { flushSync } from 'svelte';
import SampleEqGraph from './SampleEqGraph.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

beforeEach(() => {
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    );
});

function setup() {
    const draft = new EditorDraft({
        sample_eq_type: 0,
        sample_eq_frequency: 10,
        sample_eq_gain_db: -4,
        sample_eq_width_tenths: 10,
    });
    const document = {
        draft,
        detail: {
            editing: {
                blockedParameters: [],
                blockedParameterReasons: {},
                ...sampleFormatFixture(),
                eqCoefficients: [-16268, 8098, 8171, 16268, -8076],
            },
        },
    } as unknown as ObjectEditorDocument;
    const view = render(SampleEqGraph, { document, disabled: false });
    const path = (id: string) => view.container.querySelector(`[data-trace="${id}"]`)?.getAttribute('d');
    return { draft, view, path };
}

describe('Sample EQ response presentation', () => {
    it('keeps the editable low-frequency dip at the nominal gain', () => {
        const { path, view } = setup();
        const gains = path('eq')!
            .split(/[ML]/)
            .slice(1)
            .map((pair) => (1 - Number(pair.split(',')[1]) / 200) * 36 - 18);
        expect(Math.min(...gains)).toBeCloseTo(-4, 2);
        expect(Math.max(...gains)).toBeLessThanOrEqual(0);
        expect(view.container.querySelectorAll('[data-handle]').length).toBe(1);
        expect(path('coefficients')).toBeUndefined();
    });
    it('offers an explicit coefficient overlay without changing parameters or the editing curve', async () => {
        const { draft, view, path } = setup();
        const editing = path('eq');
        const toggle = view.getByRole('button', { name: 'Show coefficient response' });
        expect(toggle.getAttribute('aria-pressed')).toBe('false');
        await fireEvent.click(toggle);
        expect(toggle.getAttribute('aria-pressed')).toBe('true');
        expect(path('coefficients')).not.toBe(editing);
        expect(path('eq')).toBe(editing);
        expect(draft.dirty).toBe(false);
        const stored = path('coefficients');
        flushSync(() => draft.set('filter_cutoff', 64));
        expect(path('coefficients')).toBe(stored);
        flushSync(() => draft.set('sample_eq_gain_db', -3));
        expect(path('coefficients')).not.toBe(stored);
        expect(toggle.getAttribute('title')).toContain('draft Q13');
        flushSync(() => draft.undo());
        expect(path('coefficients')).toBe(stored);
        expect(path('eq')).toBe(editing);
        expect(toggle.getAttribute('title')).toContain('stored Q13');
        await fireEvent.click(toggle);
        expect(path('coefficients')).toBeUndefined();
        expect(draft.changes).toEqual({ filter_cutoff: 64 });
    });
});
