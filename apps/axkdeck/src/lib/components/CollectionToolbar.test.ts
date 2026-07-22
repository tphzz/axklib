import { render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CollectionToolbar from './CollectionToolbar.svelte';

describe('CollectionToolbar', () => {
    it('keeps search as the rightmost collection action', () => {
        render(CollectionToolbar, {
            props: {
                title: 'Samples (SBNK)',
                count: 1,
                query: '',
                placeholder: 'Search Samples',
                onquerychange: vi.fn(),
                actionLabel: 'Import audio',
                onaction: vi.fn(),
            },
        });

        const search = screen.getByRole('searchbox', { name: 'Search Samples' });
        const actions = search.closest('.collection-actions');
        expect(actions?.lastElementChild).toBe(search.closest('label'));
    });
});
