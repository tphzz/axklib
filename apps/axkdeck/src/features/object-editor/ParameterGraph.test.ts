import { render } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
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
});
