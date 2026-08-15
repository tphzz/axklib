import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import TreeNode from './TreeNode.svelte';

describe('TreeNode', () => {
    it('uses conventional tree arrow navigation and selects the focused row', async () => {
        const partition = {
            id: 'p0',
            name: 'Partition 0',
            kind: 'partition' as const,
            childCount: 2,
            children: [
                { id: 'v0', name: 'Piano', kind: 'volume' as const, childCount: 0 },
                { id: 'v1', name: 'Strings', kind: 'volume' as const, childCount: 0 },
            ],
        };
        const onselect = vi.fn();
        render(TreeNode, {
            props: { item: partition, selectedId: 'p0', onselect, onloadchildren: vi.fn() },
        });

        const partitionButton = screen.getByRole('button', { name: 'Partition 0' });
        partitionButton.focus();
        await fireEvent.keyDown(partitionButton, { key: 'ArrowRight' });
        expect(screen.getByRole('button', { name: /Piano \[Volume/ })).toBe(document.activeElement);
        expect(onselect).toHaveBeenLastCalledWith(partition.children[0]);

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'ArrowDown' });
        expect(screen.getByRole('button', { name: /Strings \[Volume/ })).toBe(document.activeElement);
        expect(onselect).toHaveBeenLastCalledWith(partition.children[1]);

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'Home' });
        expect(partitionButton).toBe(document.activeElement);
        await fireEvent.keyDown(partitionButton, { key: 'End' });
        expect(screen.getByRole('button', { name: /Strings \[Volume/ })).toBe(document.activeElement);

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'ArrowLeft' });
        expect(partitionButton).toBe(document.activeElement);
        expect(onselect).toHaveBeenLastCalledWith(partition);

        await fireEvent.keyDown(partitionButton, { key: 'ArrowLeft' });
        expect(screen.queryByRole('button', { name: /Piano \[Volume/ })).toBeNull();
    });

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
        const partitionButton = screen.getByRole('button', { name: 'Partition 0' });
        partitionButton.focus();
        await fireEvent.keyDown(partitionButton, { key: 'ArrowRight' });
        await waitFor(() => expect(screen.getByText('Volume α')).toBeTruthy());
        expect(onloadchildren).toHaveBeenCalledWith('p0', 0, 64);

        await fireEvent.keyDown(partitionButton, { key: 'ArrowRight' });
        expect(screen.getByRole('button', { name: /Volume α \[Volume/ })).toBe(document.activeElement);
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

    it('shows a retry action when lazy loading fails and preserves recovery', async () => {
        const onloadchildren = vi
            .fn()
            .mockRejectedValueOnce(new Error('Storage unavailable'))
            .mockResolvedValueOnce({
                items: [{ id: 'v0', name: 'Recovered', kind: 'volume', childCount: 0 }],
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

        await fireEvent.click(screen.getByRole('button', { name: 'Expand Partition 0' }));
        expect((await screen.findByRole('alert')).textContent).toContain('Storage unavailable');
        await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
        expect(await screen.findByText('Recovered')).toBeTruthy();
        expect(onloadchildren).toHaveBeenCalledTimes(2);
    });
});
