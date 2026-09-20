import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { FilesystemEntry } from '../../lib/filesystem';
import FilesInspector from './FilesInspector.svelte';

const entry: FilesystemEntry = {
    id: 'entry-first',
    parentId: 'root',
    rootId: 'root',
    ancestorIds: ['root'],
    name: 'First sample',
    path: '/First sample',
    kind: 'file',
    sizeBytes: 356,
    childCount: 0,
    objectId: 'sample-first',
    contentScopeId: null,
    interpretation: '',
    storage: 'Record 0',
    issue: '',
    filesystemMetadata: false,
    rawAttributes: 'SFS 0x94000000',
    attributes: [],
};

describe('FilesInspector expansion panels', () => {
    it('starts Properties and Storage details expanded using the same disclosure structure', () => {
        const view = render(FilesInspector, { entry });
        const ids: string[] = [];
        for (const name of ['Properties', 'Storage details']) {
            const button = view.getByRole('button', { name });
            expect(button.getAttribute('type')).toBe('button');
            expect(button.getAttribute('aria-expanded')).toBe('true');
            expect(button.closest('h4')).not.toBeNull();
            expect(button.closest('.inspector-content')).not.toBeNull();
            const id = button.getAttribute('aria-controls');
            expect(id).toBeTruthy();
            const body = document.getElementById(id!);
            expect(body).not.toBeNull();
            expect(body?.closest('[hidden], [inert]')).toBeNull();
            ids.push(id!);
        }
        expect(new Set(ids).size).toBe(2);
        expect(view.getByText('SFS 0x94000000').closest('[hidden], [inert]')).toBeNull();
    });

    it('remembers independent section choices when a different file is selected', async () => {
        const view = render(FilesInspector, { entry });
        await fireEvent.click(view.getByRole('button', { name: 'Storage details' }));

        await view.rerender({ entry: { ...entry, id: 'entry-second', name: 'Second sample', storage: 'Record 1' } });

        expect(view.getByRole('button', { name: 'Storage details' }).getAttribute('aria-expanded')).toBe('false');
        expect(view.getByRole('button', { name: 'Properties' }).getAttribute('aria-expanded')).toBe('true');
        expect(view.getByRole('heading', { name: 'Second sample' })).toBeTruthy();
        await fireEvent.click(view.getByRole('button', { name: 'Storage details' }));
        expect(view.getByText('Record 1').closest('[hidden], [inert]')).toBeNull();
    });

    it('leaves issue status and the fixed Device footer accessible when all sections are collapsed', async () => {
        const onshowdevice = vi.fn();
        const view = render(FilesInspector, {
            entry: { ...entry, issue: 'Linked Wave Data is missing.' },
            canShowDevice: true,
            onshowdevice,
        });
        await fireEvent.click(view.getByRole('button', { name: 'Properties' }));
        await fireEvent.click(view.getByRole('button', { name: 'Storage details' }));

        expect(view.getByRole('status').textContent).toBe('Linked Wave Data is missing.');
        expect(view.getByRole('status').closest('[hidden], [inert]')).toBeNull();
        const footer = view.getByRole('button', { name: 'To Device' });
        expect(footer.closest('.inspector-body')).toBeNull();
        await fireEvent.click(footer);
        expect(onshowdevice).toHaveBeenCalledWith('sample-first');
    });
});
