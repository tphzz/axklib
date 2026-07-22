import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import TreeNode from './TreeNode.svelte';

describe('TreeNode', () => {
    it('loads a branch only when it is expanded', async () => {
        const onloadchildren = vi.fn().mockResolvedValue({
            items: [{ id: 'v0', name: 'Volume α', kind: 'volume', childCount: 0 }],
            totalCount: 1,
        });
        render(TreeNode, {
            props: {
                item: { id: 'p0', name: 'Partition 0', kind: 'partition', childCount: 1 },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren,
            },
        });

        expect(onloadchildren).not.toHaveBeenCalled();
        await fireEvent.click(screen.getByRole('button', { name: 'Expand Partition 0' }));
        await waitFor(() => expect(screen.getByText('Volume α')).toBeTruthy());
        expect(onloadchildren).toHaveBeenCalledWith('p0', 0, 64);
    });

    it('treats volumes as terminal browser entries even when they contain objects', () => {
        const onloadchildren = vi.fn();
        render(TreeNode, {
            props: {
                item: { id: 'v0', name: 'Strings', kind: 'volume', childCount: 12 },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren,
            },
        });

        expect(screen.queryByRole('button', { name: 'Expand Strings' })).toBeNull();
        expect(onloadchildren).not.toHaveBeenCalled();
    });

    it('opens volume actions from pointer and keyboard context requests', async () => {
        const onrequestmenu = vi.fn();
        render(TreeNode, {
            props: {
                item: {
                    id: 'v0',
                    name: 'Strings',
                    kind: 'volume',
                    childCount: 0,
                    partitionIndex: 2,
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn(),
                volumeActionsEnabled: true,
                onrequestmenu,
            },
        });

        const item = screen.getByRole('button', { name: 'Strings [Volume]' });
        await fireEvent.contextMenu(item, { clientX: 40, clientY: 60 });
        expect(onrequestmenu).toHaveBeenLastCalledWith(expect.objectContaining({ id: 'v0' }), 40, 60);

        await fireEvent.keyDown(item, { key: 'F10', shiftKey: true });
        expect(onrequestmenu).toHaveBeenCalledTimes(2);
    });
});
