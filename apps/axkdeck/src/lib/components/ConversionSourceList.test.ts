import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ConversionSourceList from './ConversionSourceList.svelte';

const objects = [
    {
        id: 'p1',
        name: 'Stage Piano',
        kind: 'performance',
        location: 'Keys-01.img / P01',
        selectable: true,
        dependencies: ['v', 'w'],
    },
    {
        id: 'p2',
        name: 'EP Layer',
        kind: 'performance',
        location: 'Keys-02.img / P02',
        selectable: true,
        dependencies: ['v'],
    },
    { id: 'v', name: 'Piano Voice', kind: 'voice', location: 'Keys-01.img / V01', selectable: false, dependencies: [] },
    { id: 'w', name: 'PFC2', kind: 'wave', location: 'Keys-02.img / W01', selectable: false, dependencies: [] },
];

describe('Conversion source selection', () => {
    it('defaults to the whole source and excludes only the unchecked root', async () => {
        const onchange = vi.fn();
        const view = render(ConversionSourceList, { objects, selected: undefined, onchange });
        expect((view.getByRole('checkbox', { name: /All/i }) as HTMLInputElement).checked).toBe(true);
        const layer = view.getByRole('checkbox', { name: /EP Layer/ });
        expect((layer as HTMLInputElement).checked).toBe(true);
        await fireEvent.click(layer);
        expect(onchange).toHaveBeenCalledWith(['p1']);
    });

    it('distinguishes required dependencies from choices, and toggles mixed to all or all to empty', async () => {
        const onchange = vi.fn();
        const view = render(ConversionSourceList, { objects, selected: ['p1'], onchange });
        const all = () => view.getByRole('checkbox', { name: /All/i }) as HTMLInputElement;
        expect(all().indeterminate).toBe(true);
        expect(view.getByText('Piano Voice')).toBeTruthy();
        expect(view.getByText('PFC2')).toBeTruthy();
        expect(view.queryByRole('checkbox', { name: /Piano Voice|PFC2/ })).toBeNull();
        await fireEvent.click(all());
        expect(onchange).toHaveBeenLastCalledWith(undefined);
        await view.rerender({ objects, selected: undefined, onchange });
        await fireEvent.click(all());
        expect(onchange).toHaveBeenLastCalledWith([]);
    });

    it('identifies same-named wave occurrences separately across disks', async () => {
        const onchange = vi.fn();
        const waves = ['Keys-01.img', 'Keys-02.img'].map((disk, i) => ({
            id: `w${i}`,
            name: 'PFC2',
            kind: 'wave',
            location: `${disk} / W01`,
            selectable: true,
            dependencies: [],
        }));
        const view = render(ConversionSourceList, { objects: waves, selected: [], onchange });
        const second = view.getByRole('checkbox', { name: /PFC2.*Keys-02\.img/ });
        await fireEvent.click(second);
        expect(onchange).toHaveBeenCalledWith(['w1']);
    });
});
