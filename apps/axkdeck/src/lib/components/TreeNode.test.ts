/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import TreeNode from './TreeNode.svelte';

const treeNodeSource = readFileSync(resolve(process.cwd(), 'src/lib/components/TreeNode.svelte'), 'utf8');

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

    it('loads every volume page before rendering sampler display order', async () => {
        const onloadchildren = vi
            .fn()
            .mockResolvedValueOnce({
                items: [
                    { id: 'v-dollar', name: '$foo', kind: 'volume', childCount: 0 },
                    { id: 'v-a', name: 'a_foo', kind: 'volume', childCount: 0 },
                ],
                totalCount: 5,
            })
            .mockResolvedValueOnce({
                items: [
                    { id: 'v-number', name: '001_foo', kind: 'volume', childCount: 0 },
                    { id: 'v-c', name: 'c_foo', kind: 'volume', childCount: 0 },
                ],
                totalCount: 5,
            })
            .mockResolvedValueOnce({
                items: [{ id: 'v-bang', name: '!foo', kind: 'volume', childCount: 0 }],
                totalCount: 5,
            });
        const { container } = render(TreeNode, {
            props: {
                item: { id: 'p0', name: 'Partition 0', kind: 'partition', childCount: 5 },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren,
                samplerOrderingEnabled: true,
            },
        });

        await waitFor(() => expect(screen.getByText('!foo')).toBeTruthy());
        expect(onloadchildren).toHaveBeenNthCalledWith(1, 'p0', 0, 64);
        expect(onloadchildren).toHaveBeenNthCalledWith(2, 'p0', 2, 64);
        expect(onloadchildren).toHaveBeenNthCalledWith(3, 'p0', 4, 64);
        expect([...container.querySelectorAll('.tree-item-name')].map((name) => name.textContent)).toEqual([
            'Partition 0',
            '!foo',
            '$foo',
            '001_foo',
            'a_foo',
            'c_foo',
        ]);
        expect(screen.queryByRole('button', { name: /Load more/ })).toBeNull();
    });

    it('completes a partially embedded SFS volume list when its partition opens', async () => {
        const onloadchildren = vi.fn().mockResolvedValue({
            items: [{ id: 'v-bang', name: '!foo', kind: 'volume', childCount: 0 }],
            totalCount: 2,
        });
        const { container } = render(TreeNode, {
            props: {
                item: {
                    id: 'p0',
                    name: 'Partition 0',
                    kind: 'partition',
                    childCount: 2,
                    children: [{ id: 'v-a', name: 'a_foo', kind: 'volume', childCount: 0 }],
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren,
                samplerOrderingEnabled: true,
            },
        });

        await waitFor(() => expect(screen.getByText('!foo')).toBeTruthy());
        expect(onloadchildren).toHaveBeenCalledWith('p0', 1, 64);
        expect([...container.querySelectorAll('.tree-item-name')].map((name) => name.textContent)).toEqual([
            'Partition 0',
            '!foo',
            'a_foo',
        ]);
    });

    it('toggles an SFS partition when its row is double-clicked', async () => {
        render(TreeNode, {
            props: {
                item: {
                    id: 'p0',
                    name: 'Partition 0',
                    kind: 'partition',
                    childCount: 1,
                    children: [{ id: 'v0', name: 'Volume 0', kind: 'volume', childCount: 0 }],
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn(),
                samplerOrderingEnabled: true,
            },
        });

        const partition = screen.getByRole('button', { name: 'Partition 0' });
        expect(screen.getByRole('button', { name: /Volume 0 \[Volume/ })).toBeTruthy();

        await fireEvent.dblClick(partition);
        expect(screen.queryByRole('button', { name: /Volume 0 \[Volume/ })).toBeNull();

        await fireEvent.dblClick(partition);
        expect(screen.getByRole('button', { name: /Volume 0 \[Volume/ })).toBeTruthy();
    });

    it('shows partition volume and usable-capacity details with warning thresholds', () => {
        const { container } = render(TreeNode, {
            props: {
                item: {
                    id: 'p0',
                    name: 'Partition 0',
                    kind: 'partition',
                    childCount: 3,
                    partitionCapacity: {
                        allocatedClusters: 80,
                        freeClusters: 20,
                        clusterSizeBytes: 1024,
                    },
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                samplerOrderingEnabled: true,
            },
        });

        const capacity = screen.getByRole('progressbar', { name: '80% used' });
        const partitionRow = capacity.closest('.tree-row');
        const partitionStack = capacity.closest('.tree-item-stack');
        expect(capacity.getAttribute('aria-valuenow')).toBe('80');
        expect(capacity.getAttribute('aria-valuemax')).toBe('100');
        expect(capacity.classList.contains('warning')).toBe(true);
        expect(partitionRow?.classList.contains('partition-summary-row')).toBe(true);
        expect(partitionStack).toBeTruthy();
        expect(partitionStack?.querySelector('.tree-item-name')?.textContent).toBe('Partition 0');
        expect(partitionStack?.querySelector('.partition-capacity')).toBe(capacity);
        expect(container.querySelector('[role="tooltip"]')?.textContent).toContain('3 volumes');
        expect(container.querySelector('[role="tooltip"]')?.textContent).toContain('80 of 100 clusters used');
        expect(container.querySelector('[role="tooltip"]')?.textContent).toContain('80 KiB of 100 KiB used');
    });

    it('renders SFS capacity as a slim full-width second line without narrowing the partition name', () => {
        expect(treeNodeSource).toMatch(/\.partition-summary-row\s*\{[^}]*height:\s*27px/s);
        expect(treeNodeSource).toMatch(
            /\.partition-capacity\s*\{[^}]*position:\s*absolute[^}]*left:\s*0[^}]*right:\s*0[^}]*bottom:\s*1px[^}]*height:\s*3px/s,
        );
        expect(treeNodeSource).not.toContain('width: 42px');
    });

    it('marks partitions at ninety percent usage as critical', () => {
        render(TreeNode, {
            props: {
                item: {
                    id: 'p0',
                    name: 'Partition 0',
                    kind: 'partition',
                    childCount: 1,
                    partitionCapacity: {
                        allocatedClusters: 90,
                        freeClusters: 10,
                        clusterSizeBytes: 1024,
                    },
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                samplerOrderingEnabled: true,
            },
        });

        expect(screen.getByRole('progressbar', { name: '90% used' }).classList.contains('critical')).toBe(true);
    });

    it('keeps partition capacity alignment when the summary is unavailable', () => {
        const { container } = render(TreeNode, {
            props: {
                item: { id: 'p0', name: 'Partition 0', kind: 'partition', childCount: 2 },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                samplerOrderingEnabled: true,
            },
        });

        expect(screen.queryByRole('progressbar')).toBeNull();
        expect(container.querySelector('.partition-capacity.unavailable')).toBeTruthy();
        expect(container.querySelector('[role="tooltip"]')?.textContent).toContain('2 volumes');
        expect(container.querySelector('[role="tooltip"]')?.textContent).toContain('Capacity unavailable');
    });

    it('hides capacity affordances for non-SFS partitions', () => {
        const { container } = render(TreeNode, {
            props: {
                item: {
                    id: 'p0',
                    name: 'Partition 0',
                    kind: 'partition',
                    childCount: 2,
                    partitionCapacity: {
                        allocatedClusters: 80,
                        freeClusters: 20,
                        clusterSizeBytes: 1024,
                    },
                },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
                samplerOrderingEnabled: false,
            },
        });

        expect(screen.queryByRole('progressbar')).toBeNull();
        expect(container.querySelector('.partition-capacity')).toBeNull();
        expect(container.querySelector('.partition-summary-row')).toBeNull();
        expect(container.querySelector('[role="tooltip"]')).toBeNull();
        expect(screen.getByRole('button', { name: 'Partition 0' }).getAttribute('aria-describedby')).toBeNull();
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

    it('shows a compact volume size tooltip without adding a capacity bar', () => {
        const { container } = render(TreeNode, {
            props: {
                item: { id: 'v0', name: 'Strings', kind: 'volume', childCount: 12, sizeBytes: 1_572_864 },
                selectedId: '',
                onselect: vi.fn(),
                onloadchildren: vi.fn(),
            },
        });

        const volume = screen.getByRole('button', { name: 'Strings [Volume]' });
        const tooltip = screen.getByRole('tooltip');
        expect(tooltip.textContent).toContain('Size: 1.5 MiB');
        expect(volume.getAttribute('aria-describedby')).toBe(tooltip.id);
        expect(container.querySelector('.partition-capacity')).toBeNull();
    });

    it('does not keep a tooltip visible merely because a pointer-selected row retains focus', () => {
        expect(treeNodeSource).not.toContain('.tree-row:focus-within > .tree-item-tooltip');
        expect(treeNodeSource).toContain('.tree-item-select:focus-visible ~ .tree-item-tooltip');
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
