import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import LibrarySidebar from './LibrarySidebar.svelte';

describe('LibrarySidebar', () => {
    it('shows canonical names with compact type and partition metadata', () => {
        render(LibrarySidebar, {
            props: {
                items: [
                    {
                        id: 'partition-0',
                        name: 'PARTITION 1',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 0,
                    },
                    {
                        id: 'volume-1',
                        name: 'drumloops',
                        kind: 'volume',
                        partitionIndex: 0,
                        childCount: 0,
                    },
                ],
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                volumeActionsEnabled: true,
                partitionActionsEnabled: true,
                onimageaction: vi.fn(),
            },
        });

        expect(screen.getByRole('button', { name: 'PARTITION 1 [Partition 0]' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'drumloops [Volume]' })).toBeTruthy();
        expect(screen.getByRole('searchbox', { name: 'Search volumes' }).getAttribute('placeholder')).toBe('Search');
    });

    it('offers partition rename independently from volume mutations', async () => {
        const onimageaction = vi.fn();
        const partition = {
            id: 'partition-0',
            name: 'PARTITION 1',
            kind: 'partition' as const,
            partitionIndex: 0,
            childCount: 0,
        };
        render(LibrarySidebar, {
            props: {
                items: [partition],
                selectedId: partition.id,
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                volumeActionsEnabled: false,
                partitionActionsEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: 'PARTITION 1 [Partition 0]' }));
        expect(screen.queryByRole('menuitem', { name: 'Add volume' })).toBeNull();
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename partition' }));
        expect(onimageaction).toHaveBeenCalledWith(partition, 'rename-partition');
    });
});
