import { render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CollectionToolbar from './CollectionToolbar.svelte';

describe('CollectionToolbar', () => {
    it('keeps search as the rightmost collection action', () => {
        render(CollectionToolbar, {
            props: {
                title: 'Samples',
                count: 1,
                query: '',
                onquerychange: vi.fn(),
                actionLabel: 'Import audio',
                actionIcon: 'upload',
                onaction: vi.fn(),
            },
        });

        const search = screen.getByRole('searchbox', { name: 'Search Samples' });
        expect(search.getAttribute('placeholder')).toBe('Search');
        const actions = search.closest('.collection-actions');
        expect(actions?.lastElementChild).toBe(search.closest('label'));
    });

    it('renders a cleaning action with the requested icon and accessible label', () => {
        const onaction = vi.fn();
        render(CollectionToolbar, {
            props: {
                title: 'Wave Data',
                count: 3,
                query: '',
                onquerychange: vi.fn(),
                actionLabel: 'Clean up unreferenced Wave Data',
                actionIcon: 'broom',
                onaction,
            },
        });

        const action = screen.getByRole('button', { name: 'Clean up unreferenced Wave Data' });
        expect(action.querySelector('[data-icon="broom"]')).toBeTruthy();
        action.click();
        expect(onaction).toHaveBeenCalledOnce();
    });
});
