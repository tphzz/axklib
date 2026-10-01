import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ParameterGraph from './ParameterGraph.svelte';

describe('parameter plot viewport', () => {
    it('hides out-of-view handles instead of drawing them beyond the plot border', () => {
        const handle = { id: 'end', label: 'Release', readout: '0', x: 1.02, y: 0.5, vertical: true };
        const view = render(ParameterGraph, {
            label: 'Envelope',
            traces: [],
            ticks: [127, 0, -127],
            axis: [],
            handles: [handle, { ...handle, id: 'start', label: 'Initial', x: 0 }],
        });
        expect(view.queryByRole('button', { name: 'Release: 0' })).toBeNull();
        expect(view.getByRole('button', { name: 'Initial: 0' })).toBeTruthy();
    });
    it('changes the selected overlapping handle without changing the trace or calling edit callbacks', async () => {
        const props = {
            label: 'Overlapping stages',
            handles: [
                { id: 'initial', label: 'Initial', readout: '0', x: 0, y: 0.5, vertical: true },
                { id: 'attack', label: 'Attack', readout: '0', x: 0, y: 0.5, vertical: true },
            ],
            traces: [
                {
                    id: 'envelope',
                    points: [
                        { x: 0, y: 0.5 },
                        { x: 1, y: 0.5 },
                    ],
                },
            ],
            ticks: [],
            axis: [],
            onchange: vi.fn(),
            onbegin: vi.fn(),
            retainReadout: true,
        };
        const view = render(ParameterGraph, { ...props, selected: 'initial' });
        const initial = view.getByRole('button', { name: 'Initial: 0' });
        const attack = view.getByRole('button', { name: 'Attack: 0' });
        const trace = view.container.querySelector('[data-trace="envelope"]')!.getAttribute('d');
        expect(initial.getAttribute('aria-pressed')).toBe('true');
        await view.rerender({ ...props, selected: 'attack' });
        expect(attack.getAttribute('aria-pressed')).toBe('true');
        expect(initial.getAttribute('aria-pressed')).toBe('false');
        expect(view.container.querySelector('output')!.textContent).toBe('Attack: 0');
        expect(view.container.querySelector('[data-trace="envelope"]')!.getAttribute('d')).toBe(trace);
        expect(initial.getAttribute('style')).toBe(attack.getAttribute('style'));
        expect(props.onchange).not.toHaveBeenCalled();
        expect(props.onbegin).not.toHaveBeenCalled();
    });
    it('keeps transient readouts as the default for other graph consumers', async () => {
        const view = render(ParameterGraph, {
            label: 'Graph',
            handles: [{ id: 'point', label: 'Point', readout: '0', x: 0, y: 0.5, vertical: true }],
            traces: [],
            ticks: [],
            axis: [],
        });
        const handle = view.getByRole('button', { name: 'Point: 0' });
        await fireEvent.focus(handle);
        await fireEvent.pointerEnter(handle);
        expect(view.container.querySelector('output')!.textContent).toBe('Point: 0');
        await fireEvent.pointerLeave(handle);
        expect(view.container.querySelector('output')!.textContent).toBe('');
        expect(handle.getAttribute('aria-pressed')).toBe('true');
    });
});
