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
    it('identifies reserved entries without a false warning or invented permissions', async () => {
        const view = render(FilesInspector, { entry });
        expect(view.getByText('Filesystem metadata')).toBeTruthy();
        expect(view.getByText('SFS 0x94000000')).toBeTruthy();
        expect(view.getByText('SFS 0x94000000').closest('details')?.open).toBe(false);
        expect(view.getByRole('region', { name: 'Entry properties' }).textContent).not.toContain('0x');
        expect(view.queryByRole('status')).toBeNull();
        expect(view.queryByText('Read-only')).toBeNull();
        expect(view.queryByRole('button', { name: 'Show in Device' })).toBeNull();
        await fireEvent.click(view.getByText('Storage details'));
        await fireEvent.focus(view.getByRole('button', { name: 'Native attributes' }));
        expect(view.getByRole('tooltip').textContent?.trim()).toBe(
            'Filesystem-specific technical storage information. The hexadecimal value contains the stored attribute and type encoding.',
        );
    });

    it('uses the same properties layout for native FAT flags', () => {
        const view = render(FilesInspector, {
            entry: {
                ...entry,
                name: 'FILE.WAV',
                filesystemMetadata: false,
                rawAttributes: 'FAT 0x23',
                attributes: ['Read-only', 'Hidden', 'Archive'].map((label) => ({
                    code: `fat.${label.toLowerCase()}`,
                    label,
                    value: 'Yes',
                    summary: label,
                    description: '',
                })),
            },
        });
        expect(view.getByText('FAT 0x23')).toBeTruthy();
        expect(view.getByRole('region', { name: 'Entry properties' }).textContent).toContain('Read-only');
        expect(view.queryByText('Filesystem metadata')).toBeNull();
        expect(view.getByRole('region', { name: 'Entry properties' }).querySelectorAll('dl').length).toBe(1);
    });

    it('shows compact attributes once with explanations available through label help', async () => {
        const view = render(FilesInspector, {
            entry: {
                ...entry,
                attributes: [
                    {
                        code: 'sfs.file-write',
                        label: 'File write flag',
                        value: 'Disabled',
                        summary: 'File write flag: Disabled',
                        description: 'Controls ordinary file data writes and extension.',
                    },
                    {
                        code: 'sfs.references',
                        label: 'Filesystem references',
                        value: '1',
                        summary: '',
                        description: 'Number of filesystem references to this record.',
                    },
                ],
            },
        });
        const properties = view.getByRole('region', { name: 'Entry properties' });
        expect(properties.textContent).toContain('File write flag');
        expect(properties.textContent).toContain('Disabled');
        expect(properties.textContent).not.toContain('Filesystem references');
        expect(view.queryByText('Read-only')).toBeNull();
        expect(view.getByText('Filesystem references').closest('details')?.open).toBe(false);
        expect(view.getAllByText('File write flag')).toHaveLength(1);
        expect(view.queryByText('Controls ordinary file data writes and extension.')).toBeNull();
        const label = view.getByRole('button', { name: 'File write flag' });
        await fireEvent.focus(label);
        expect(view.getByRole('tooltip').textContent).toContain('Controls ordinary file data writes');
        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(view.queryByRole('tooltip')).toBeNull();
    });
});
