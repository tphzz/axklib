import { cleanup, fireEvent, render } from '@testing-library/svelte';
import { createRawSnippet, tick } from 'svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import GraphPanel from './GraphPanel.svelte';
import { graphLayout } from './graphLayout.svelte';

const measured = vi.hoisted(() => ({ width: 850, resize: (_width: number) => {} }));
vi.mock('./measureWidth', () => ({
    measureWidth: (_node: HTMLElement, { change }: { change?: (width: number) => void }) => {
        if (change) {
            measured.resize = change;
            change(measured.width);
        }
        return { destroy() {} };
    },
}));

const graph = createRawSnippet(() => ({ render: () => '<span>Graph</span>' }));
const controls = createRawSnippet(() => ({ render: () => '<span>Controls</span>' }));
const programLayout = {
    graph,
    controls,
    label: 'Program Amp EG',
    layoutKey: 'program-amp',
    controlsMinimum: 300,
    stackBelow: 740,
};

beforeEach(() => {
    measured.width = 850;
    graphLayout.ratio = 0.5;
    graphLayout.views = {};
});
afterEach(cleanup);

describe('graph panel layout boundaries', () => {
    it('keeps Program graph and controls side by side at 850 CSS pixels and stacks below 740', async () => {
        const view = render(GraphPanel, programLayout);
        await tick();
        expect(view.getByRole('separator', { name: 'Resize graph and controls' })).toBeTruthy();
        expect(view.container.querySelector('.graph-panel.stacked')).toBeNull();
        measured.resize(740);
        await tick();
        expect(view.queryByRole('separator')).not.toBeNull();
        measured.resize(739);
        await tick();
        expect(view.queryByRole('separator')).toBeNull();
        expect(view.container.querySelector('.graph-panel.stacked')).not.toBeNull();
    });

    it('preserves the Sample graph breakpoint when no Program layout is requested', async () => {
        const view = render(GraphPanel, { graph, controls, label: 'Sample' });
        await tick();
        expect(view.queryByRole('separator')).toBeNull();
        measured.resize(900);
        await tick();
        expect(view.queryByRole('separator')).not.toBeNull();
    });

    it('keeps Program resizing independent from Sample and other Program pages', async () => {
        graphLayout.ratio = 0.6;
        const view = render(GraphPanel, programLayout);
        await tick();
        await fireEvent.keyDown(view.getByRole('separator'), { key: 'End' });
        expect(graphLayout.views['program-amp']).toBeCloseTo(1 - 300 / 842);
        expect(graphLayout.ratio).toBe(0.6);
        await view.rerender({ layoutKey: 'program-routing', graphMinimum: 420 });
        expect(view.getByRole('separator').getAttribute('aria-valuenow')).toBe('50');
        await fireEvent.keyDown(view.getByRole('separator'), { key: 'Home' });
        expect(graphLayout.views['program-routing']).toBeCloseTo(420 / 842);
        expect(graphLayout.views['program-amp']).toBeCloseTo(1 - 300 / 842);
        expect(graphLayout.ratio).toBe(0.6);
    });
});
