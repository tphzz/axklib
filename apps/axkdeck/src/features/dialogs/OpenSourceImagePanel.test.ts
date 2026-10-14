import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import OpenSourceImagePanel from './OpenSourceImagePanel.svelte';
import { storagePickerDirectory, storagePickerFixture } from '../../test/storagePickerFixture';

function setup(extra = {}, longList = false) {
    const onselectsources = vi.fn();
    const view = render(OpenSourceImagePanel, {
        transport: storagePickerFixture(longList),
        mode: 'media-source',
        onselect: vi.fn(),
        oncancel: vi.fn(),
        onselectsources,
        ...extra,
    });
    return { ...view, onselectsources };
}
const sourceList = () => within(screen.getByRole('region', { name: 'Source set' }));

describe('Explicit source selection', () => {
    it('immediately toggles files and synchronizes removals without an intermediate footer', async () => {
        const { container, onselectsources } = setup();
        expect(screen.getByRole('button', { name: 'Open' })).toHaveProperty('disabled', true);
        await fireEvent.click(await screen.findByRole('option', { name: /Yamaha images/ }));
        const file = await screen.findByRole('option', { name: /Disk02.hds/ });
        await fireEvent.click(file);
        expect(sourceList().getAllByRole('listitem')).toHaveLength(1);
        expect(file.getAttribute('aria-selected')).toBe('true');
        expect(screen.queryByRole('button', { name: /^Add \d/ })).toBeNull();
        expect(container.querySelectorAll('.dialog-footer')).toHaveLength(1);
        await fireEvent.click(file);
        expect(sourceList().getByText('No sources selected')).toBeTruthy();
        await fireEvent.click(file);
        await fireEvent.click(sourceList().getByRole('button', { name: /Remove .*Disk02.hds/ }));
        expect(file.getAttribute('aria-selected')).toBe('false');
        await fireEvent.click(file);
        await fireEvent.click(screen.getByRole('button', { name: 'Open' }));
        expect(onselectsources).toHaveBeenCalledExactlyOnceWith([
            expect.objectContaining({
                kind: 'server-file',
                reference: { rootId: 'workspace', relativePath: 'Drum Kits/Disk02.hds' },
            }),
        ]);
    });

    it('retains ordered selection across folders and pagination and toggles the current folder explicitly', async () => {
        const { onselectsources } = setup();
        await fireEvent.click(await screen.findByRole('option', { name: /Yamaha images/ }));
        await fireEvent.click(await screen.findByRole('option', { name: /Disk02.hds/ }));
        await fireEvent.click(screen.getByRole('button', { name: 'Load more' }));
        await fireEvent.click(await screen.findByRole('option', { name: /Disk10.hds/ }));
        await fireEvent.click(screen.getByRole('option', { name: /ACE-FR2L/ }));
        expect(sourceList().getAllByRole('listitem')).toHaveLength(2);
        await fireEvent.click(screen.getByRole('button', { name: 'Add current folder' }));
        expect(screen.getByRole('button', { name: 'Remove current folder' }).getAttribute('aria-pressed')).toBe('true');
        await fireEvent.click(screen.getByRole('button', { name: 'Remove current folder' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Add current folder' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Go to all workspaces' }));
        await fireEvent.click(screen.getByRole('option', { name: /Yamaha images/ }));
        expect((await screen.findByRole('option', { name: /Disk02.hds/ })).getAttribute('aria-selected')).toBe('true');
        await fireEvent.click(screen.getByRole('button', { name: 'Open' }));
        expect(
            onselectsources.mock.calls[0]?.[0].map(
                (item: { reference: { relativePath: string } }) => item.reference.relativePath,
            ),
        ).toEqual(['Drum Kits/Disk02.hds', 'Drum Kits/Disk10.hds', 'Drum Kits/ACE-FR2L']);
    });

    it('restores focus without selecting and only toggles on Enter or Space', async () => {
        setup({
            initialDirectory: storagePickerDirectory,
            initialFile: { rootId: 'workspace', relativePath: 'Drum Kits/Disk02.hds' },
        });
        const file = await screen.findByRole('option', { name: /Disk02.hds/ });
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        expect(file.getAttribute('aria-selected')).toBe('false');
        await fireEvent.focus(list);
        await fireEvent.keyDown(list, { key: 'ArrowDown' });
        expect(sourceList().getByText('No sources selected')).toBeTruthy();
        await fireEvent.keyDown(list, { key: 'Enter' });
        expect(sourceList().getByText(/disk2.hds/)).toBeTruthy();
        await fireEvent.keyDown(list, { key: ' ' });
        expect(sourceList().getByText('No sources selected')).toBeTruthy();
    });

    it('rejects a 33rd source without changing membership and keeps removals available', async () => {
        const { onselectsources } = setup({}, true);
        await fireEvent.click(await screen.findByRole('option', { name: /Yamaha images/ }));
        const files = await screen.findAllByRole('option');
        for (const file of files.slice(0, 32)) await fireEvent.click(file);
        await fireEvent.click(files[32]!);
        expect(sourceList().getAllByRole('listitem')).toHaveLength(32);
        expect(files[32]!.getAttribute('aria-selected')).toBe('false');
        expect(screen.getByRole('status').textContent).toContain('at most 32');
        await fireEvent.click(files[0]!);
        await fireEvent.click(files[32]!);
        expect(screen.getByRole('status').textContent).toBe('32 sources selected');
        await fireEvent.click(screen.getByRole('button', { name: 'Open' }));
        expect(onselectsources.mock.calls[0]?.[0]).toHaveLength(32);
    });
});
