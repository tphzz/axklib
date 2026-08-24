import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { DiskTreeItem } from '../types';
import ImageTreeContextMenu from './ImageTreeContextMenu.svelte';

const partition = {
    id: 'partition-1',
    kind: 'partition',
    name: 'Partition 1',
} as DiskTreeItem;

const volume = {
    id: 'volume-1',
    kind: 'volume',
    name: 'Strings',
    partitionIndex: 0,
} as DiskTreeItem;

const common = {
    item: partition,
    left: 240,
    top: 180,
    volumeActionsEnabled: true,
    partitionActionsEnabled: true,
    packageImportEnabled: true,
    packageExportEnabled: true,
    volumePackageExportEnabled: true,
    volumeFloppyExportEnabled: true,
    audioExportEnabled: true,
    mediaConversionEnabled: true,
    allocationInspectionEnabled: true,
    onaction: vi.fn(),
    onclose: vi.fn(),
};

describe('ImageTreeContextMenu', () => {
    it('renders the root menu only at its viewport-clamped position', () => {
        const { container } = render(ImageTreeContextMenu, { props: common });

        const menu = container.querySelector<HTMLDivElement>('[aria-label="Partition 1 actions"]');
        expect(menu).not.toBeNull();
        if (!menu) throw new Error('root menu was not rendered');
        expect(menu.style.visibility).toBe('visible');
        expect(menu.style.pointerEvents).toBe('auto');
        expect(menu.style.left).toBe('240px');
        expect(menu.style.top).toBe('180px');
    });

    it('keeps a submenu hidden until it has been positioned beside its parent', async () => {
        const { container } = render(ImageTreeContextMenu, { props: common });
        const root = container.querySelector<HTMLDivElement>('[aria-label="Partition 1 actions"]');
        expect(root).not.toBeNull();
        if (!root) throw new Error('root menu was not rendered');
        await waitFor(() => expect(root.style.visibility).toBe('visible'));

        await fireEvent.mouseEnter(screen.getByRole('menuitem', { name: 'Import' }));

        const submenu = container.querySelector<HTMLDivElement>('[aria-label="Import actions"]');
        expect(submenu).not.toBeNull();
        if (!submenu) throw new Error('submenu was not rendered');
        await waitFor(() => expect(submenu.style.visibility).toBe('visible'));
        expect(submenu.style.pointerEvents).toBe('auto');
        expect(submenu.style.left).not.toBe('0px');
    });

    it('offers only one plural delete command for a volume multi-selection', async () => {
        const onaction = vi.fn();
        render(ImageTreeContextMenu, {
            props: { ...common, item: volume, selectionCount: 3, onaction },
        });

        const action = screen.getByRole('menuitem', { name: 'Delete 3 volumes…' });
        expect(screen.getAllByRole('menuitem')).toEqual([action]);
        await fireEvent.click(action);
        expect(onaction).toHaveBeenCalledWith('delete-volume');
    });
});
