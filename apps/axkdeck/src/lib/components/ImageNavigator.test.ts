/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../storageLocations';
import ImageNavigator from './ImageNavigator.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

const common = {
    items: [],
    selectedId: '',
    opening: false,
    storageLocationsAvailable: true,
    onopen: vi.fn(),
    oncreate: vi.fn(),
    onclose: vi.fn(),
    onmanagelocations: vi.fn(),
    onselect: vi.fn(),
    onloadchildren: vi.fn().mockResolvedValue({ items: [], totalCount: 0 }),
    volumeActionsEnabled: false,
    partitionActionsEnabled: false,
    onimageaction: vi.fn(),
};

describe('ImageNavigator', () => {
    it('presents compact open and create actions with quiet empty contents', async () => {
        const onopen = vi.fn();
        const oncreate = vi.fn();
        const onmanagelocations = vi.fn();
        const { container } = render(ImageNavigator, {
            props: { ...common, image: null, onopen, oncreate, onmanagelocations },
        });

        expect(screen.getByRole('complementary', { name: 'Image navigator' })).toBeTruthy();
        expect(screen.getByText('Contents')).toBeTruthy();
        expect(screen.queryByText('Volumes')).toBeNull();
        expect(screen.queryByText('Open an existing sampler image or create a new one.')).toBeNull();
        expect(screen.queryByRole('searchbox', { name: 'Search image contents' })).toBeNull();
        expect(screen.getByText('Open an image to browse its contents')).toBeTruthy();

        const emptyActions = container.querySelector('.image-empty-actions');
        expect(emptyActions).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Open image' }).closest('.image-empty-actions')).toBe(emptyActions);
        expect(screen.getByRole('button', { name: 'Create image' }).closest('.image-empty-actions')).toBe(emptyActions);

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Create image' }));
        expect(onopen).toHaveBeenCalledOnce();
        expect(oncreate).toHaveBeenCalledOnce();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Storage locations' }));
        expect(onmanagelocations).toHaveBeenCalledOnce();
    });

    it('places replacement and eject actions on the loaded image row', async () => {
        const onopen = vi.fn();
        const oncreate = vi.fn();
        const onclose = vi.fn();
        const onintegrity = vi.fn();
        const onmanagelocations = vi.fn();
        const { container } = render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation(
                    { rootId: 'workspace', relativePath: 'images/nested.hds' },
                    'Yamaha/images/nested.hds',
                ),
                onopen,
                oncreate,
                onclose,
                onintegrity,
                onmanagelocations,
            },
        });

        expect(screen.getByText('nested.hds')).toBeTruthy();
        expect(screen.getByText('Yamaha/images')).toBeTruthy();
        const imageRow = container.querySelector('.active-image');
        expect(imageRow).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Open another image' }).closest('.active-image')).toBe(imageRow);
        expect(screen.getByRole('button', { name: 'Eject image' }).closest('.active-image')).toBe(imageRow);

        await fireEvent.click(screen.getByRole('button', { name: 'Open another image' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Eject image' }));
        expect(onopen).toHaveBeenCalledOnce();
        expect(onclose).toHaveBeenCalledOnce();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Image integrity...' }));
        expect(onintegrity).toHaveBeenCalledOnce();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Create new image' }));
        expect(oncreate).toHaveBeenCalledOnce();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Storage locations' }));
        expect(onmanagelocations).toHaveBeenCalledOnce();
    });

    it('disables loaded-image lifecycle controls while a replacement is opening', () => {
        render(ImageNavigator, {
            props: {
                ...common,
                opening: true,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'nested.hds' }, 'Yamaha/nested.hds'),
            },
        });

        expect((screen.getByRole('button', { name: 'Open another image' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Eject image' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Image options' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('shows the disk children as contents without repeating the active image root', () => {
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'nested.hds' }, 'Yamaha/nested.hds'),
                items: [
                    {
                        id: 'disk',
                        name: 'nested.hds',
                        kind: 'disk',
                        childCount: 1,
                        children: [{ id: 'partition', name: 'Partition 0', kind: 'partition', childCount: 0 }],
                    },
                ],
            },
        });

        expect(screen.getAllByText('nested.hds')).toHaveLength(1);
        expect(screen.getByText('Partition 0')).toBeTruthy();
        expect(screen.getByRole('searchbox', { name: 'Search image contents' })).toBeTruthy();
    });

    it('renders SFS partitions in sampler display order and preserves physical selection identity', async () => {
        const onselect = vi.fn();
        const { container } = render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                samplerOrderingEnabled: true,
                onselect,
                items: [
                    {
                        id: 'disk',
                        name: 'disk.hds',
                        kind: 'disk',
                        childCount: 4,
                        children: [
                            { id: 'p0', name: '001_PARTITION 1', kind: 'partition', partitionIndex: 0, childCount: 0 },
                            { id: 'p2', name: 'A_PARTITION 3', kind: 'partition', partitionIndex: 2, childCount: 0 },
                            { id: 'p3', name: '_PARTITION 4', kind: 'partition', partitionIndex: 3, childCount: 0 },
                            { id: 'p7', name: '$PARTITION 8', kind: 'partition', partitionIndex: 7, childCount: 0 },
                        ],
                    },
                ],
            },
        });

        expect([...container.querySelectorAll('.tree-item-name')].map((name) => name.textContent)).toEqual([
            '$PARTITION 8',
            '001_PARTITION 1',
            'A_PARTITION 3',
            '_PARTITION 4',
        ]);

        await fireEvent.click(screen.getByRole('button', { name: '$PARTITION 8 [Partition 7]' }));
        expect(onselect).toHaveBeenCalledWith(expect.objectContaining({ id: 'p7', partitionIndex: 7 }), 'replace', []);
    });

    it('does not reorder partitions when sampler ordering is disabled', () => {
        const { container } = render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'library.iso' }),
                items: [
                    { id: 'p3', name: '_PARTITION 4', kind: 'partition', partitionIndex: 3, childCount: 0 },
                    { id: 'p7', name: '$PARTITION 8', kind: 'partition', partitionIndex: 7, childCount: 0 },
                ],
            },
        });

        expect([...container.querySelectorAll('.tree-item-name')].map((name) => name.textContent)).toEqual([
            '_PARTITION 4',
            '$PARTITION 8',
        ]);
    });

    it('preserves sampler-significant repeated spaces in volume names', () => {
        const { container } = render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'library.iso' }),
                items: [
                    {
                        id: 'disk',
                        name: 'library.iso',
                        kind: 'disk',
                        childCount: 1,
                        children: [
                            {
                                id: 'volume',
                                name: '14 S.E.    /2.3M',
                                kind: 'volume',
                                childCount: 0,
                            },
                        ],
                    },
                ],
            },
        });

        const label = container.querySelector('.tree-item-name') as HTMLElement;
        expect(label).toBeTruthy();
        expect(label.textContent).toBe('14 S.E.    /2.3M');
        expect(getComputedStyle(label).whiteSpace).toBe('pre');
    });

    it('loads a lazy disk root while keeping that technical root out of the navigator', async () => {
        const onloadchildren = vi.fn().mockResolvedValue({
            items: [{ id: 'volume', name: 'Piano', kind: 'volume', childCount: 0 }],
            totalCount: 1,
        });
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'nested.hds' }, 'Yamaha/nested.hds'),
                items: [{ id: 'disk', name: 'nested.hds', kind: 'disk', childCount: 1 }],
                onloadchildren,
            },
        });

        expect(await screen.findByText('Piano')).toBeTruthy();
        expect(screen.getAllByText('nested.hds')).toHaveLength(1);
        expect(onloadchildren).toHaveBeenCalledWith('disk', 0, 200);
    });

    it('offers package actions for a volume independently from mutation availability', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'nested.hds' }),
                items: [
                    {
                        id: 'disk',
                        name: 'nested.hds',
                        kind: 'disk',
                        childCount: 1,
                        children: [
                            {
                                id: 'volume',
                                name: 'DRUMS',
                                kind: 'volume',
                                childCount: 0,
                                partitionIndex: 0,
                            },
                        ],
                    },
                ],
                packageImportEnabled: true,
                packageExportEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }), {
            clientX: 20,
            clientY: 20,
        });
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import package…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'import-package');

        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }), {
            clientX: 20,
            clientY: 20,
        });
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export volume package…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'export-package');
        expect(screen.queryByRole('menuitem', { name: 'Delete volume' })).toBeNull();
    });

    it('groups volume workflows and expert tools with consistent ordering and separators', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                items: [
                    {
                        id: 'volume',
                        name: 'DRUMS',
                        kind: 'volume',
                        childCount: 0,
                        partitionIndex: 0,
                        volumeDirectoryId: 17,
                    },
                ],
                packageImportEnabled: true,
                packageExportEnabled: true,
                audioExportEnabled: true,
                mediaConversionEnabled: true,
                volumeActionsEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }));
        const rootMenu = screen.getByRole('menu', { name: 'DRUMS actions' });
        expect(
            within(rootMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Import', 'Export', 'Rename volume…', 'Delete volume', 'Expert']);
        expect(rootMenu.querySelectorAll(':scope > [role="separator"]')).toHaveLength(2);

        await fireEvent.click(within(rootMenu).getByRole('menuitem', { name: 'Export' }));
        const exportMenu = screen.getByRole('menu', { name: 'Export actions' });
        expect(
            within(exportMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Export volume package…', 'Export floppy image…', 'Export SFZ…']);

        await fireEvent.click(within(rootMenu).getByRole('menuitem', { name: 'Expert' }));
        const expertMenu = screen.getByRole('menu', { name: 'Expert actions' });
        expect(within(expertMenu).getByRole('menuitem', { name: 'Repair object placement…' })).toBeTruthy();
    });

    it('offers only volume package export for a read-only AXK object directory', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation(
                    { rootId: 'workspace', relativePath: 'floppies/FS1R' },
                    'Yamaha/floppies/FS1R',
                ),
                items: [
                    {
                        id: 'disk',
                        name: 'FS1R',
                        kind: 'disk',
                        childCount: 1,
                        children: [
                            {
                                id: 'object-directory-volume',
                                name: 'Object directory',
                                kind: 'volume',
                                childCount: 3,
                                partitionIndex: 0,
                            },
                        ],
                    },
                ],
                packageExportEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: /Object directory/ }));
        expect(screen.queryByRole('menuitem', { name: 'Import' })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Rename volume…' })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Delete volume' })).toBeNull();
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export volume package…' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'object-directory-volume', partitionIndex: 0 }),
            'export-package',
        );
    });

    it('groups partition import and export workflows and orders the remaining actions', async () => {
        const onimageaction = vi.fn();
        const { container } = render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'library.iso' }),
                items: [
                    {
                        id: 'partition-0',
                        name: 'SYNTHS',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 1,
                    },
                ],
                packageImportEnabled: true,
                volumePackageExportEnabled: true,
                volumeFloppyExportEnabled: true,
                mediaConversionEnabled: true,
                allocationInspectionEnabled: true,
                partitionActionsEnabled: true,
                volumeActionsEnabled: true,
                onimageaction,
            },
        });

        const partitionButton = screen.getByText('SYNTHS').closest('button');
        expect(partitionButton).not.toBeNull();
        const treeScroll = container.querySelector('.image-tree-scroll');
        expect(treeScroll).not.toBeNull();
        await fireEvent.contextMenu(partitionButton!);
        expect(treeScroll?.classList.contains('context-menu-open')).toBe(true);
        const rootMenu = screen.getByRole('menu', { name: 'SYNTHS actions' });
        expect(
            within(rootMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Import', 'Export', 'Rename partition…', 'Add volume…', 'Expert']);
        expect(rootMenu.querySelectorAll(':scope > [role="separator"]')).toHaveLength(2);
        const menuGeometry = appStyles.match(/\.tree-context-menu\s*\{[^}]+\}/)?.[0];
        const menuActionGeometry = appStyles.match(/\.tree-context-menu button\s*\{[^}]+\}/)?.[0];
        expect(menuGeometry).toBeDefined();
        expect(menuActionGeometry).toBeDefined();
        const style = document.createElement('style');
        style.textContent = `${menuGeometry}\n${menuActionGeometry}`;
        document.head.append(style);
        const menuStyle = getComputedStyle(rootMenu);
        expect(menuStyle.width).toBe('220px');
        await fireEvent.click(within(rootMenu).getByRole('menuitem', { name: 'Export' }));
        const exportMenu = screen.getByRole('menu', { name: 'Export actions' });
        expect(
            within(exportMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Export volume packages…', 'Export volumes to floppies…', 'Export CD-ROM image…']);
        const floppyActionStyle = getComputedStyle(
            within(exportMenu).getByRole('menuitem', { name: 'Export volumes to floppies…' }),
        );
        expect(floppyActionStyle.whiteSpace).toBe('nowrap');
        style.remove();
        await fireEvent.click(within(exportMenu).getByRole('menuitem', { name: 'Export volumes to floppies…' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'partition-0' }),
            'export-volume-floppies',
        );
        await fireEvent.contextMenu(partitionButton!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import packages…' }));
        expect(treeScroll?.classList.contains('context-menu-open')).toBe(false);
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'partition-0' }), 'import-packages');
        await fireEvent.contextMenu(partitionButton!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Expert' }));
        const expertMenu = screen.getByRole('menu', { name: 'Expert actions' });
        expect(
            within(expertMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Visualize partition allocation', 'Repair object placement…']);
        await fireEvent.click(within(expertMenu).getByRole('menuitem', { name: 'Visualize partition allocation' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'partition-0' }),
            'inspect-allocation',
        );
        await fireEvent.contextMenu(partitionButton!);
        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(treeScroll?.classList.contains('context-menu-open')).toBe(false);
        expect(screen.queryByRole('menu')).toBeNull();
        expect(appStyles).toMatch(/\.image-tree-scroll\.context-menu-open\s*\{[^}]*scrollbar-width:\s*none;[^}]*\}/);
        expect(appStyles).toMatch(
            /\.image-tree-scroll\.context-menu-open::-webkit-scrollbar\s*\{[^}]*display:\s*none;[^}]*\}/,
        );
    });

    it('supports keyboard traversal into and out of partition submenus', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                items: [
                    {
                        id: 'partition',
                        name: 'PARTITION 1',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 0,
                    },
                ],
                packageImportEnabled: true,
                volumePackageExportEnabled: true,
                volumeFloppyExportEnabled: true,
                allocationInspectionEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByText('PARTITION 1').closest('button')!);
        const importParent = screen.getByRole('menuitem', { name: 'Import' });
        expect(document.activeElement).toBe(importParent);
        await fireEvent.keyDown(importParent, { key: 'ArrowRight' });
        const importLeaf = screen.getByRole('menuitem', { name: 'Import packages…' });
        expect(document.activeElement).toBe(importLeaf);
        await fireEvent.keyDown(importLeaf, { key: 'ArrowLeft' });
        expect(document.activeElement).toBe(importParent);
        expect(screen.queryByRole('menu', { name: 'Import actions' })).toBeNull();
        await fireEvent.keyDown(importParent, { key: 'ArrowDown' });
        const exportParent = screen.getByRole('menuitem', { name: 'Export' });
        expect(document.activeElement).toBe(exportParent);
        await fireEvent.keyDown(exportParent, { key: 'End' });
        const expertParent = screen.getByRole('menuitem', { name: 'Expert' });
        expect(document.activeElement).toBe(expertParent);
        await fireEvent.keyDown(expertParent, { key: 'ArrowRight' });
        const expertLeaf = screen.getByRole('menuitem', { name: 'Visualize partition allocation' });
        expect(document.activeElement).toBe(expertLeaf);
        await fireEvent.keyDown(expertLeaf, { key: 'ArrowLeft' });
        expect(document.activeElement).toBe(expertParent);
    });

    it('does not render empty partition workflow submenus', async () => {
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                items: [
                    {
                        id: 'partition',
                        name: 'PARTITION 1',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 0,
                    },
                ],
                partitionActionsEnabled: true,
            },
        });

        await fireEvent.contextMenu(screen.getByText('PARTITION 1').closest('button')!);
        expect(screen.queryByRole('menuitem', { name: 'Import' })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Export' })).toBeNull();
        expect(screen.getAllByRole('separator')).toHaveLength(1);
    });

    it('offers CD-ROM conversion for partitions and floppy conversion for addressable volumes', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                items: [
                    {
                        id: 'disk',
                        name: 'disk.hds',
                        kind: 'disk',
                        childCount: 1,
                        children: [
                            {
                                id: 'partition',
                                name: 'PARTITION 1',
                                kind: 'partition',
                                partitionIndex: 0,
                                childCount: 1,
                                children: [
                                    {
                                        id: 'volume',
                                        name: 'DRUMS',
                                        kind: 'volume',
                                        partitionIndex: 0,
                                        volumeDirectoryId: 17,
                                        childCount: 0,
                                    },
                                ],
                            },
                        ],
                    },
                ],
                mediaConversionEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByText('PARTITION 1').closest('button')!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export CD-ROM image…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'partition' }), 'export-cdrom');

        await fireEvent.click(screen.getByRole('button', { name: /Expand PARTITION 1/ }));
        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export floppy image…' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'volume', volumeDirectoryId: 17 }),
            'export-floppy',
        );
    });

    it('offers explicit placement repair on writable partitions and volumes', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                items: [
                    {
                        id: 'partition',
                        name: 'PARTITION 1',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 1,
                        children: [
                            {
                                id: 'volume',
                                name: 'DRUMS',
                                kind: 'volume',
                                partitionIndex: 0,
                                childCount: 0,
                            },
                        ],
                    },
                ],
                partitionActionsEnabled: true,
                volumeActionsEnabled: true,
                onimageaction,
            },
        });

        await fireEvent.contextMenu(screen.getByText('PARTITION 1').closest('button')!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Expert' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Repair object placement…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'partition' }), 'repair-placement');

        await fireEvent.click(screen.getByRole('button', { name: /Expand PARTITION 1/ }));
        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Expert' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Repair object placement…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'repair-placement');
    });

    it('preserves a selected volume set for context deletion and replaces it for an unselected row', async () => {
        const onselect = vi.fn();
        const oncontextselect = vi.fn();
        const onimageaction = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
                selectedId: 'v1',
                selectedVolumeIds: ['v0', 'v1'],
                items: [
                    {
                        id: 'partition',
                        name: 'PARTITION 1',
                        kind: 'partition',
                        partitionIndex: 0,
                        childCount: 3,
                        children: [
                            { id: 'v0', name: 'Piano', kind: 'volume', partitionIndex: 0, childCount: 0 },
                            { id: 'v1', name: 'Strings', kind: 'volume', partitionIndex: 0, childCount: 0 },
                            { id: 'v2', name: 'Brass', kind: 'volume', partitionIndex: 0, childCount: 0 },
                        ],
                    },
                ],
                samplerOrderingEnabled: true,
                volumeActionsEnabled: true,
                onselect,
                oncontextselect,
                onimageaction,
            },
        });

        const piano = await screen.findByRole('button', { name: 'Piano [Volume]' });
        const strings = screen.getByRole('button', { name: 'Strings [Volume]' });
        const brass = screen.getByRole('button', { name: 'Brass [Volume]' });
        expect(piano.getAttribute('aria-pressed')).toBe('true');
        expect(strings.getAttribute('aria-pressed')).toBe('true');
        expect(brass.getAttribute('aria-pressed')).toBe('false');

        await fireEvent.contextMenu(piano, { clientX: 40, clientY: 60 });
        expect(oncontextselect).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'v0' }),
            expect.arrayContaining([
                expect.objectContaining({ id: 'v0' }),
                expect.objectContaining({ id: 'v1' }),
                expect.objectContaining({ id: 'v2' }),
            ]),
        );
        expect(screen.getAllByRole('menuitem')).toHaveLength(1);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete 2 volumes…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'v0' }), 'delete-volume');

        await fireEvent.contextMenu(brass, { clientX: 50, clientY: 70 });
        expect(onselect).toHaveBeenLastCalledWith(
            expect.objectContaining({ id: 'v2' }),
            'replace',
            expect.arrayContaining([
                expect.objectContaining({ id: 'v0' }),
                expect.objectContaining({ id: 'v1' }),
                expect.objectContaining({ id: 'v2' }),
            ]),
        );
        expect(screen.queryByRole('menuitem', { name: 'Delete 2 volumes…' })).toBeNull();
        expect(screen.getByRole('menuitem', { name: 'Rename volume…' })).toBeTruthy();
    });

    it('does not advertise deletion for a multi-selection on read-only media', async () => {
        const oncontextselect = vi.fn();
        render(ImageNavigator, {
            props: {
                ...common,
                image: serverFileLocation({ rootId: 'workspace', relativePath: 'library.iso' }),
                selectedId: 'v1',
                selectedVolumeIds: ['v0', 'v1'],
                items: [
                    { id: 'v0', name: 'Piano', kind: 'volume', partitionIndex: 0, childCount: 0 },
                    { id: 'v1', name: 'Strings', kind: 'volume', partitionIndex: 0, childCount: 0 },
                ],
                packageExportEnabled: true,
                oncontextselect,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: 'Piano [Volume]' }));

        expect(oncontextselect).toHaveBeenCalledOnce();
        expect(screen.queryByRole('menu')).toBeNull();
        expect(screen.queryByRole('menuitem', { name: /Delete/ })).toBeNull();
    });
});
