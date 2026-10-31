import { act, fireEvent, render } from '@testing-library/svelte';
import { createRawSnippet } from 'svelte';
import { describe, expect, it } from 'vitest';
import ParameterGroups from './ParameterGroups.svelte';

describe('compact parameter-group navigation', () => {
    it('uses shared content tabs and retains field-group selection', async () => {
        const view = render(ParameterGroups, {
            groups: [
                { title: 'Filter', fields: ['Cutoff'] },
                { title: 'Velocity', fields: ['Depth'] },
            ],
            field: createRawSnippet(() => ({ render: () => '<span>Parameter</span>' })),
        });
        const navigation = view.getByRole('navigation', { name: 'Parameter groups' });
        expect(navigation.getAttribute('data-appearance')).toBe('content');
        const first = view.getByRole('button', { name: 'Filter' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        await act(() => {});
        expect(view.getByRole('button', { name: 'Velocity' }).getAttribute('aria-pressed')).toBe('true');
        expect(view.container.querySelector('section.active h3')?.textContent).toBe('Velocity');
    });
});
