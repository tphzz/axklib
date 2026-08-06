import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../storageLocations';
import ImageNavigator from './ImageNavigator.svelte';

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
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import package…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'import-package');

        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }), {
            clientX: 20,
            clientY: 20,
        });
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'export-package');
        expect(screen.queryByRole('menuitem', { name: 'Delete volume' })).toBeNull();
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
        expect(screen.queryByRole('menuitem', { name: 'Import package…' })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Rename volume' })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Delete volume' })).toBeNull();
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'object-directory-volume', partitionIndex: 0 }),
            'export-package',
        );
    });

    it('offers batch volume-package export on an addressable partition', async () => {
        const onimageaction = vi.fn();
        render(ImageNavigator, {
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
                volumePackageExportEnabled: true,
                onimageaction,
            },
        });

        const partitionButton = screen.getByText('SYNTHS').closest('button');
        expect(partitionButton).not.toBeNull();
        await fireEvent.contextMenu(partitionButton!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export volume packages…' }));
        expect(onimageaction).toHaveBeenCalledWith(
            expect.objectContaining({ id: 'partition-0' }),
            'export-volume-packages',
        );
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
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export CD-ROM image…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'partition' }), 'export-cdrom');

        await fireEvent.click(screen.getByRole('button', { name: /Expand PARTITION 1/ }));
        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }));
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
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Repair object placement…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'partition' }), 'repair-placement');

        await fireEvent.click(screen.getByRole('button', { name: /Expand PARTITION 1/ }));
        await fireEvent.contextMenu(screen.getByRole('button', { name: /DRUMS/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Repair object placement…' }));
        expect(onimageaction).toHaveBeenCalledWith(expect.objectContaining({ id: 'volume' }), 'repair-placement');
    });
});
