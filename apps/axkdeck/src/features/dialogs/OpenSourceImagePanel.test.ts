import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import OpenSourceImagePanel from './OpenSourceImagePanel.svelte';
import { storagePickerFixture } from '../../test/storagePickerFixture';

describe('Explicit source selection', () => {
    it('collects files and directories, deduplicates, removes and opens the exact set', async () => {
        const onselectsources = vi.fn();
        render(OpenSourceImagePanel, {
            transport: storagePickerFixture(),
            mode: 'media-source',
            onselect: vi.fn(),
            oncancel: vi.fn(),
            onselectsources,
        });
        expect(screen.getByRole('button', { name: 'Open' })).toHaveProperty('disabled', true);
        await fireEvent.click(await screen.findByRole('option', { name: /Yamaha images/ }));
        await fireEvent.click(await screen.findByRole('option', { name: /Disk02.hds/ }));
        await fireEvent.click(screen.getByRole('button', { name: 'Add 1 file' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Add 1 file' }));
        const list = screen.getByRole('region', { name: 'Source set' });
        expect(within(list).getAllByRole('listitem')).toHaveLength(1);
        await fireEvent.click(screen.getByRole('option', { name: /Disk02.hds/ }), { ctrlKey: true });
        await fireEvent.click(screen.getByRole('button', { name: 'Add current folder' }));
        expect(within(list).getAllByRole('listitem')).toHaveLength(2);
        await fireEvent.click(within(list).getAllByRole('button')[0]!);
        await fireEvent.click(screen.getByRole('button', { name: 'Open' }));
        expect(onselectsources).toHaveBeenCalledOnce();
        expect(onselectsources.mock.calls[0]?.[0]).toEqual([
            expect.objectContaining({ kind: 'server-directory', reference: { rootId: 'workspace', relativePath: '' } }),
        ]);
    });
});
