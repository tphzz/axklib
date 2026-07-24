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
});
