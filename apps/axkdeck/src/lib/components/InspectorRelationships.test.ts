import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import InspectorRelationships from './InspectorRelationships.svelte';

describe('InspectorRelationships', () => {
    it('renders grouped relationships and navigates only resolvable objects', async () => {
        const onnavigate = vi.fn();
        render(InspectorRelationships, {
            props: {
                groups: [
                    {
                        objectType: 'SBAC',
                        label: 'Sample Banks',
                        items: [
                            {
                                id: 'known',
                                objectId: 'bank-1',
                                name: 'Piano Bank',
                                detail: 'Assignment · =Smp',
                                navigable: true,
                            },
                            {
                                id: 'unknown',
                                name: 'Missing Bank',
                                detail: 'Assignment',
                                navigable: false,
                            },
                        ],
                    },
                ],
                onnavigate,
            },
        });

        const relationship = screen.getByRole('button', { name: 'Piano Bank Assignment · =Smp' });
        await fireEvent.click(relationship, { detail: 1 });
        await fireEvent.click(relationship, { detail: 0 });

        expect(onnavigate).toHaveBeenNthCalledWith(1, 'bank-1', false);
        expect(onnavigate).toHaveBeenNthCalledWith(2, 'bank-1', true);
        expect(screen.getByTitle('Not resolvable').textContent).toContain('Missing Bank');
        expect(screen.queryByRole('button', { name: /Missing Bank/ })).toBeNull();
    });

    it('shows an explicit empty state', () => {
        render(InspectorRelationships, { props: { groups: [] } });

        expect(screen.getByText('No direct relationships')).toBeTruthy();
    });
});
