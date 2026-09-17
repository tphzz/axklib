import { describe, expect, it, vi, afterEach } from 'vitest';
import { cleanup, fireEvent, render } from '@testing-library/svelte';
import FilesImageImportDialog from './FilesImageImportDialog.svelte';
import { FilesImageImportWorkflow, isFilesystemImageDrop } from './imageImportWorkflow.svelte';
import { imageContentRows, selectedImageRows } from './imageImportSelection';
import type {
    FilesystemImageInspection,
    FilesystemInputSnapshot,
    FilesystemMutationDriver,
} from '../../lib/filesystem';
import type { ClientFilesystemImportEntry, FilesystemImportActions } from '../../lib/filesystemImport';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import { serverFileLocation } from '../../lib/storageLocations';
import type { JobState } from '../../lib/transport';

export const imageCapabilities = {
    ...writableFilesRoot,
    namePolicy: 'FAT_8_3_UPPERCASE' as const,
    maximumNameBytes: 12,
    supportedImports: ['FAT_FLOPPY_CONTENTS'],
};
afterEach(cleanup);
const snapshot: FilesystemInputSnapshot = { revision: 'file', sizeBytes: 42, sha256: 'a'.repeat(64) };
const completed: JobState = { jobId: 9, kind: 'inspect', status: 'completed' };
export const contents: FilesystemImageInspection = {
    inspectionToken: 'b'.repeat(64),
    entries: [
        { entryId: 'dir', relativePath: ['DEMOS'], directory: true, snapshot: null },
        { entryId: 'one', relativePath: ['DEMOS', 'ONE.S1A'], directory: false, snapshot },
        { entryId: 'two', relativePath: ['DEMOS', 'TWO.S1A'], directory: false, snapshot },
        { entryId: 'empty', relativePath: ['EMPTY'], directory: true, snapshot: null },
    ],
};

export function imageImportSetup(count = 1) {
    const sources = Array.from({ length: count }, (_, index) =>
        serverFileLocation({ rootId: 'disk', relativePath: `disk${index}.ima` }),
    );
    const entries: ClientFilesystemImportEntry[] = sources.map((_, index) => ({
        directory: false,
        relativePath: [`disk${index}.ima`],
        source: { name: `disk${index}.ima`, size: 10000, type: '', readChunk: vi.fn() },
    }));
    const imports = {
        supportsClientUploads: true,
        chooseFiles: vi.fn(),
        chooseDirectory: vi.fn(),
        upload: vi.fn<FilesystemImportActions['upload']>().mockResolvedValue(sources),
        release: vi.fn<FilesystemImportActions['release']>().mockResolvedValue(),
        inspectInputs: vi.fn<FilesystemImportActions['inspectInputs']>().mockResolvedValue({
            ...completed,
            result: { inputs: sources.map((source) => ({ source: { fileRef: source.reference }, snapshot })) },
        }),
        images: {
            inspect: vi
                .fn<NonNullable<FilesystemImportActions['images']>['inspect']>()
                .mockResolvedValue({ ...completed, result: contents }),
            release: vi.fn().mockResolvedValue(undefined),
        },
        inspectDestination: vi
            .fn<FilesystemImportActions['inspectDestination']>()
            .mockImplementation(async (revision, parentEntryId, rows) => ({
                ...completed,
                result: {
                    imageId: 'image',
                    revision,
                    parentEntryId,
                    conflictCount: 0,
                    entries: rows.map((row) => ({
                        ...row,
                        action: row.directory ? 'CREATE_DIRECTORY' : 'CREATE_FILE',
                        existingSizeBytes: null,
                        issue: '',
                    })),
                },
            })),
        observe: vi.fn(),
        cancel: vi.fn().mockResolvedValue(undefined),
    };
    const mutations = {
        execute: vi
            .fn<FilesystemMutationDriver['execute']>()
            .mockResolvedValue({ ...completed, result: { imageId: 'image', revision: 4, warnings: [] } }),
        observe: vi.fn(),
        cancel: vi.fn(),
        refresh: vi.fn().mockResolvedValue(undefined),
    };
    const status = vi.fn();
    const workflow = new FilesImageImportWorkflow(status);
    const open = () => workflow.open(3, filesystemEntry(), imageCapabilities, imports, mutations, entries);
    return { workflow, imports, mutations, entries, sources, status, open };
}

describe('Filesystem floppy imports', () => {
    it('presents tri-state selection, shared controls and the standard footer without auto-closing on failure', async () => {
        const { workflow, open } = imageImportSetup();
        await open();
        const view = render(FilesImageImportDialog, { workflow });
        expect(view.getByRole('button', { name: 'Contents' }).getAttribute('aria-pressed')).toBe('true');
        expect((view.getByRole('checkbox', { name: 'Select all entries' }) as HTMLInputElement).checked).toBe(true);
        await fireEvent.click(view.getByRole('button', { name: 'Collapse DEMOS' }));
        expect(view.queryByRole('checkbox', { name: 'Import disk0.ima: DEMOS/ONE.S1A' })).toBeNull();
        expect(workflow.allSelected).toBe(true);
        await fireEvent.click(view.getByRole('button', { name: 'Expand DEMOS' }));
        expect(
            [...view.getByRole('dialog').querySelectorAll('footer button')].map((button) => button.textContent?.trim()),
        ).toEqual(['Cancel', 'Review', 'Import']);
        await fireEvent.click(view.getByRole('checkbox', { name: 'Import disk0.ima: DEMOS/ONE.S1A' }));
        expect(
            (view.getByRole('checkbox', { name: 'Import disk0.ima: DEMOS' }) as HTMLInputElement).indeterminate,
        ).toBe(true);
        expect((view.getByRole('checkbox', { name: 'Select all entries' }) as HTMLInputElement).indeterminate).toBe(
            true,
        );
        await fireEvent.click(view.getByRole('button', { name: 'File' }));
        expect((view.getByRole('textbox', { name: 'Filename 1.1' }) as HTMLInputElement).value).toBe('DISK0.IMA');
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        expect(view.queryByRole('dialog')).toBeNull();
    });
    it('routes only image-only top-level drops', () => {
        const { entries } = imageImportSetup();
        expect(isFilesystemImageDrop(entries)).toBe(true);
        expect(isFilesystemImageDrop([...entries, { directory: true, relativePath: ['dir'] }])).toBe(false);
        expect(isFilesystemImageDrop([{ ...entries[0], relativePath: ['dir', 'disk.ima'] }])).toBe(false);
        expect(isFilesystemImageDrop([])).toBe(false);
    });
    it('defaults to contents, preserves mode edits and requires review after any change', async () => {
        const { workflow, open, imports } = imageImportSetup();
        await open();
        expect(workflow.mode).toBe('Contents');
        expect(workflow.importer.canSubmit).toBe(false);
        workflow.rename(0, 1, 'other.s1a');
        await workflow.review();
        expect(workflow.importer.canSubmit).toBe(true);
        workflow.setMode('File');
        expect(workflow.importer.rows[0].name).toBe('DISK0.IMA');
        expect(workflow.importer.canSubmit).toBe(false);
        workflow.rename(0, 0, 'copy.ima');
        workflow.setMode('Contents');
        expect(workflow.importer.rows[1].name).toBe('OTHER.S1A');
        workflow.setMode('File');
        expect(workflow.importer.rows[0].name).toBe('COPY.IMA');
        expect(imports.upload).toHaveBeenCalledOnce();
        workflow.close();
    });
    it('selects descendants and includes required parents without an image-name wrapper', async () => {
        const { workflow, open, mutations } = imageImportSetup();
        await open();
        workflow.selectAll(false);
        workflow.toggle(0, 1, true);
        expect(workflow.selection(0, 0)).toBe('some');
        expect(workflow.importer.rows.map((row) => row.name)).toEqual(['DEMOS', 'ONE.S1A']);
        await workflow.review();
        await workflow.importer.submit();
        const edits = mutations.execute.mock.calls[0][1];
        expect(edits).toEqual([
            { kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['DEMOS'] },
            {
                kind: 'PUT_FILE',
                parentEntryId: 'folder',
                relativePath: ['DEMOS', 'ONE.S1A'],
                source: {
                    kind: 'image-entry',
                    displayName: 'DEMOS/ONE.S1A',
                    reference: { inspectionToken: contents.inspectionToken, entryId: 'one' },
                },
                expectedSource: snapshot,
                conflict: 'SKIP',
            },
        ]);
        expect(workflow.importer.target).toBeNull();
    });
    it('blocks overlapping files from different images but merges shared directories', async () => {
        const { workflow, open, mutations } = imageImportSetup(2);
        await open();
        expect(workflow.errors.size).toBe(4);
        expect(workflow.canReview).toBe(false);
        await workflow.review();
        expect(mutations.execute).not.toHaveBeenCalled();
        workflow.toggle(1, 1, false);
        workflow.toggle(1, 2, false);
        expect(workflow.errors.size).toBe(0);
        expect(workflow.importer.rows.filter((row) => row.directory).map((row) => row.name)).toEqual([
            'DEMOS',
            'EMPTY',
        ]);
        expect(workflow.canReview).toBe(true);
        workflow.close();
    });
    it('keeps malformed content errors visible while allowing a raw image import', async () => {
        const { workflow, imports, open } = imageImportSetup();
        imports.images.inspect.mockResolvedValue({ ...completed, status: 'failed', error: 'Invalid FAT geometry' });
        await open();
        expect(workflow.groups[0].error).toBe('Invalid FAT geometry');
        expect(workflow.importer.target).not.toBeNull();
        expect(workflow.canReview).toBe(false);
        workflow.setMode('File');
        expect(workflow.canReview).toBe(true);
        workflow.close();
    });
    it('cleans every inspection and original upload before refresh and never repeats a committed write', async () => {
        const { workflow, imports, mutations, open } = imageImportSetup();
        await open();
        await workflow.review();
        mutations.refresh.mockRejectedValueOnce(new Error('Offline'));
        await workflow.importer.submit();
        expect(workflow.importer.phase).toBe('refresh-failed');
        expect(imports.images.release).toHaveBeenCalledWith(contents.inspectionToken);
        expect(imports.release.mock.invocationCallOrder[0]).toBeLessThan(mutations.refresh.mock.invocationCallOrder[0]);
        await workflow.importer.submit();
        expect(mutations.execute).toHaveBeenCalledOnce();
        expect(workflow.importer.target).toBeNull();
    });
    it('releases inspections which complete after cancellation', async () => {
        const { workflow, imports, open } = imageImportSetup();
        let finish!: (job: JobState) => void;
        imports.images.inspect.mockImplementation(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const pending = open();
        await vi.waitFor(() => expect(finish).toBeTypeOf('function'));
        workflow.close();
        finish({ ...completed, result: contents });
        await pending;
        expect(imports.images.release).toHaveBeenCalledWith(contents.inspectionToken);
        expect(workflow.importer.target).toBeNull();
    });
    it('rejects unsafe hierarchy and duplicate file-directory paths', () => {
        expect(() =>
            imageContentRows(
                { ...contents, entries: [{ ...contents.entries[1], relativePath: ['..', 'FILE'] }] },
                imageCapabilities,
            ),
        ).toThrow();
        const rows = imageContentRows(contents, imageCapabilities);
        const prepared = selectedImageRows([
            { name: 'one', rows, selected: [0, 1], error: '' },
            { name: 'two', rows: [{ ...rows[1], name: 'DEMOS', parent: null }], selected: [0], error: '' },
        ]);
        expect(prepared.errors.size).toBe(2);
    });
});
