import { cleanup, fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { FilesImportWorkflow } from './importWorkflow.svelte';
import FilesImportDialog from './FilesImportDialog.svelte';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import { serverFileLocation } from '../../lib/storageLocations';
import type { FilesystemImportActions } from '../../lib/filesystemImport';

afterEach(cleanup);
function setup() {
    const source = serverFileLocation({ rootId: 'host', relativePath: 'tone.bin' });
    const job = { jobId: 1, kind: 'review', status: 'completed' as const };
    const imports: FilesystemImportActions = {
        supportsClientUploads: true,
        chooseDirectory: vi.fn().mockResolvedValue(null),
        chooseFiles: vi.fn().mockResolvedValue([source]),
        upload: vi.fn(),
        release: vi.fn().mockResolvedValue(undefined),
        inspectInputs: vi.fn().mockResolvedValue({
            ...job,
            result: {
                inputs: [
                    {
                        source: { fileRef: source.reference },
                        snapshot: { revision: 'rev', sizeBytes: 42, sha256: 'a'.repeat(64) },
                    },
                ],
            },
        }),
        inspectDestination: vi.fn().mockImplementation(async (revision, parentEntryId, entries) => ({
            ...job,
            result: {
                imageId: 'image',
                revision,
                parentEntryId,
                conflictCount: 0,
                entries: entries.map((entry: { directory: boolean }) => ({
                    ...entry,
                    action: entry.directory ? 'CREATE_DIRECTORY' : 'CREATE_FILE',
                    issue: '',
                    existingSizeBytes: null,
                })),
            },
        })),
        observe: vi.fn(),
        cancel: vi.fn(),
    };
    const workflow = new FilesImportWorkflow();
    workflow.open(3, filesystemEntry(), writableFilesRoot, imports, {
        execute: vi.fn(),
        observe: vi.fn(),
        cancel: vi.fn(),
        refresh: vi.fn(),
    });
    return { workflow, imports, view: render(FilesImportDialog, { workflow }) };
}

describe('Files import dialog', () => {
    it('describes dropped folders without claiming contents-only import', async () => {
        const { workflow, view } = setup();
        await workflow.chooseDropped(async () => [{ directory: true, relativePath: ['DROP'] }]);
        expect(view.getByText('Includes dropped folders and their contents.')).toBeTruthy();
        expect(view.queryByText(/Folder contents only/)).toBeNull();
    });
    it('shows directory context and disables file conflict policies for directories', async () => {
        const { workflow, imports, view } = setup();
        vi.mocked(imports.chooseDirectory).mockResolvedValue([
            { relativePath: ['a'], directory: true },
            { relativePath: ['a', 'b'], directory: true },
        ]);
        await fireEvent.click(view.getByRole('button', { name: 'Choose folder...' }));
        await waitFor(() => expect(workflow.phase).toBe('ready'));
        expect(view.getByRole('dialog', { name: 'Import from disk' })).toBeTruthy();
        expect(view.queryAllByRole('combobox')).toHaveLength(0);
        expect(view.getByText('2 entries')).toBeTruthy();
        expect(view.getByText('a')).toBeTruthy();
        await fireEvent.input(view.getByRole('textbox', { name: 'Filename 1' }), { target: { value: 'renamed' } });
        expect(view.getByText('renamed')).toBeTruthy();
        expect(workflow.phase).toBe('dirty');
    });
    it('bounds editable DOM rows without dropping entries from the review', async () => {
        const { view, workflow } = setup();
        await workflow.chooseWorkspace();
        const first = workflow.rows[0];
        workflow.rows = Array.from({ length: 201 }, (_, index) => ({ ...first, name: `file${index}` }));
        await waitFor(() => expect(view.getAllByRole('textbox')).toHaveLength(100));
        await fireEvent.click(view.getByRole('button', { name: 'Next page' }));
        expect(view.getByRole('textbox', { name: 'Filename 101' })).toBeTruthy();
        expect(workflow.rows).toHaveLength(201);
        await fireEvent.click(view.getByRole('button', { name: 'Next page' }));
        expect(view.getAllByRole('textbox')).toHaveLength(1);
        await fireEvent.click(view.getByRole('button', { name: 'Previous page' }));
        expect(view.getAllByRole('textbox')).toHaveLength(100);
    });
    it('reuses the source chooser and reviews selected paths before enabling import', async () => {
        const { view, workflow } = setup();
        expect(view.getByRole('dialog', { name: 'Add files' })).toBeTruthy();
        expect((view.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.click(view.getByRole('button', { name: /Storage location/ }));
        await waitFor(() => expect(workflow.phase).toBe('ready'));
        expect((view.getByRole('textbox', { name: 'Filename 1' }) as HTMLInputElement).value).toBe('tone.bin');
        expect((view.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(false);
        expect(view.getByText(/Relationships are not repaired/)).toBeTruthy();
    });
    it('uses shared controls and requires review again after a filename or policy changes', async () => {
        const { view, workflow } = setup();
        await workflow.chooseWorkspace();
        await waitFor(() => expect(view.getByRole('textbox', { name: 'Filename 1' })).toBeTruthy());
        const input = view.getByRole('textbox', { name: 'Filename 1' });
        expect(input.classList.contains('dialog-field-control')).toBe(true);
        await fireEvent.input(input, { target: { value: 'new.bin' } });
        expect(workflow.phase).toBe('dirty');
        expect((view.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.change(view.getByRole('combobox', { name: 'If file exists 1' }), {
            target: { value: 'REPLACE' },
        });
        await fireEvent.click(view.getByRole('button', { name: 'Review' }));
        await waitFor(() => expect(workflow.phase).toBe('ready'));
        expect(workflow.rows[0].conflict).toBe('REPLACE');
    });
    it('keeps mixed footer buttons at matching shared geometry', () => {
        const css = readFileSync('src/app.css', 'utf8');
        const style = document.createElement('style');
        style.textContent = [
            css.match(/\.secondary-button,\s*\.primary-button,\s*\.danger-button\s*\{[^}]+\}/)?.[0],
            css.match(
                /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
            )?.[0],
        ].join('\n');
        document.head.append(style);
        try {
            const { view } = setup();
            for (const name of ['Cancel', 'Review', 'Import']) {
                const computed = getComputedStyle(view.getByRole('button', { name }));
                expect(computed.height).toBe('30px');
                expect(computed.marginTop).toBe('0px');
                expect(computed.marginBottom).toBe('0px');
            }
        } finally {
            style.remove();
        }
    });
    it('closes on Escape and returns initial focus to Cancel', async () => {
        const { view, workflow } = setup();
        await waitFor(() => expect(document.activeElement).toBe(view.getByRole('button', { name: 'Cancel' })));
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        expect(workflow.target).toBeNull();
    });
});
