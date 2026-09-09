import { render, cleanup, fireEvent } from '@testing-library/svelte';
import { afterEach, describe, expect, it } from 'vitest';
import type { FilesystemEntry, FilesystemAccess } from '../../lib/filesystem';
import { FilesController } from './controller.svelte';
import FilesView from './FilesView.svelte';

afterEach(cleanup);

describe('FilesView native metadata', () => {
    it('mutes metadata while keeping it selectable and aligns the attributes column', async () => {
        const root: FilesystemEntry = {
            id: 'root',
            parentId: null,
            rootId: 'root',
            ancestorIds: [],
            name: 'Disk',
            path: '',
            kind: 'root',
            sizeBytes: null,
            childCount: 1,
            objectId: null,
            contentScopeId: null,
            interpretation: '',
            storage: '',
            issue: '',
            filesystemMetadata: false,
            rawAttributes: '',
            attributes: [],
        };
        const file: FilesystemEntry = {
            ...root,
            id: 'support',
            parentId: 'root',
            ancestorIds: ['root'],
            name: 'sfserrlog',
            kind: 'file',
            filesystemMetadata: true,
            childCount: 0,
        };
        const access: FilesystemAccess = {
            inspect: async (query) => ({
                revision: 1,
                available: true,
                filesystemName: 'SFS',
                deviceView: null,
                items: query?.parentId ? [file] : [root],
                totalCount: 1,
                rootCapabilities: [],
            }),
        };
        const controller = new FilesController(access);
        await controller.initialize();
        const view = render(FilesView, { controller });
        const row = view.getByRole('row');
        expect(row.classList.contains('filesystem-metadata')).toBe(true);
        expect(view.getByText('Metadata')).toBeTruthy();
        expect(view.getByText('Attributes')).toBeTruthy();
        expect(view.getByRole('treegrid').contains(view.getByText('Attributes'))).toBe(true);
        expect(row.querySelectorAll('[role="gridcell"]').length).toBe(4);
        await fireEvent.click(row);
        expect(controller.selected?.id).toBe(file.id);
        expect(row.getAttribute('aria-selected')).toBe('true');
    });
});
