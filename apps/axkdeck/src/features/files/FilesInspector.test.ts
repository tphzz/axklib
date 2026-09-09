import { render, cleanup, fireEvent } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { FilesystemEntry } from '../../lib/filesystem';
import FilesInspector from './FilesInspector.svelte';

afterEach(cleanup);

const entry: FilesystemEntry = {
    id: 'entry',
    parentId: 'root',
    rootId: 'root',
    ancestorIds: ['root'],
    name: 'sfserram',
    path: '/sfserram',
    kind: 'file',
    sizeBytes: 256,
    childCount: 0,
    objectId: null,
    contentScopeId: null,
    interpretation: '',
    storage: 'Record 0',
    issue: '',
    filesystemMetadata: true,
    rawAttributes: 'SFS 0x94000000',
    attributes: [],
};

describe('FilesInspector native metadata', () => {
    it('keeps cross-mode navigation in the fixed footer outside scrolling properties', async () => {
        const onshowdevice = vi.fn();
        const view = render(FilesInspector, {
            entry: { ...entry, objectId: 'object' },
            canShowDevice: true,
            onshowdevice,
        });
        const button = view.getByRole('button', { name: 'To Device' });
        expect(button.closest('.inspector-body')).toBeNull();
        expect(button.closest('.inspector-mode-footer')).toBeTruthy();
        expect(button.getAttribute('title')).toBe('Show this entry in Device');
        await fireEvent.click(button);
        expect(onshowdevice).toHaveBeenCalledWith('object');
    });
    it('identifies reserved entries without a false warning or invented permissions', () => {
        const view = render(FilesInspector, { entry });
        expect(view.getByText('Filesystem metadata')).toBeTruthy();
        expect(view.getByText('SFS 0x94000000')).toBeTruthy();
        expect(view.queryByRole('status')).toBeNull();
        expect(view.queryByText('Read-only')).toBeNull();
        expect(view.queryByRole('button', { name: 'Show in Device' })).toBeNull();
    });

    it('uses the same properties layout for native FAT flags', () => {
        const view = render(FilesInspector, {
            entry: {
                ...entry,
                name: 'FILE.WAV',
                filesystemMetadata: false,
                rawAttributes: 'FAT 0x23',
                attributes: ['Read-only', 'Hidden', 'Archive'],
            },
        });
        expect(view.getByText('FAT 0x23')).toBeTruthy();
        expect(view.getByText('Read-only, Hidden, Archive')).toBeTruthy();
        expect(view.queryByText('Filesystem metadata')).toBeNull();
        expect(view.getByRole('region', { name: 'Entry properties' }).querySelectorAll('dl').length).toBe(1);
    });
});
