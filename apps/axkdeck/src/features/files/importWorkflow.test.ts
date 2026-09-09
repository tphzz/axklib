import { describe, expect, it, vi } from 'vitest';
import { FilesImportWorkflow } from './importWorkflow.svelte';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import { serverFileLocation } from '../../lib/storageLocations';
import type { FilesystemMutationDriver, FilesystemInputSnapshot } from '../../lib/filesystem';
import type { FilesystemImportActions } from '../../lib/filesystemImport';
import type { JobState } from '../../lib/transport';

const snapshot: FilesystemInputSnapshot = { revision: 'source-version', sizeBytes: 42, sha256: 'a'.repeat(64) };
const source = serverFileLocation({ rootId: 'disk', relativePath: 'source.bin' });
const completed: JobState = { jobId: 9, kind: 'images.filesystem.edit', status: 'completed' };

function setup(capabilities = writableFilesRoot) {
    const imports = {
        supportsClientUploads: true,
        chooseDirectory: vi.fn<FilesystemImportActions['chooseDirectory']>().mockResolvedValue(null),
        chooseFiles: vi.fn<FilesystemImportActions['chooseFiles']>().mockResolvedValue([source]),
        upload: vi.fn<FilesystemImportActions['upload']>().mockResolvedValue([source]),
        release: vi.fn<FilesystemImportActions['release']>().mockResolvedValue(),
        inspectInputs: vi.fn<FilesystemImportActions['inspectInputs']>().mockResolvedValue({
            ...completed,
            result: { inputs: [{ source: { fileRef: source.reference }, snapshot }] },
        }),
        inspectDestination: vi
            .fn<FilesystemImportActions['inspectDestination']>()
            .mockImplementation(async (revision, parentEntryId, entries) => ({
                ...completed,
                result: {
                    imageId: 'image',
                    revision,
                    parentEntryId,
                    conflictCount: 0,
                    entries: entries.map((entry) => ({
                        ...entry,
                        action: entry.directory ? 'CREATE_DIRECTORY' : 'CREATE_FILE',
                        existingSizeBytes: null,
                        issue: '',
                    })),
                },
            })),
        observe: vi.fn<FilesystemImportActions['observe']>().mockResolvedValue(completed),
        cancel: vi.fn<FilesystemImportActions['cancel']>().mockResolvedValue(),
    };
    const mutations = {
        execute: vi.fn<FilesystemMutationDriver['execute']>().mockResolvedValue(completed),
        observe: vi.fn<FilesystemMutationDriver['observe']>().mockResolvedValue(completed),
        cancel: vi.fn<FilesystemMutationDriver['cancel']>().mockResolvedValue(),
        refresh: vi.fn<FilesystemMutationDriver['refresh']>().mockResolvedValue(),
    };
    const workflow = new FilesImportWorkflow();
    expect(workflow.open(3, filesystemEntry(), capabilities, imports, mutations)).toBe(true);
    return { workflow, imports, mutations };
}

describe('Files import workflow', () => {
    it('retains completion warnings until dismissed without permitting a second write', async () => {
        const { workflow, mutations } = setup();
        await workflow.chooseWorkspace();
        mutations.execute.mockResolvedValue({
            ...completed,
            result: { imageId: 'image', revision: 4, warnings: ['Check imported files'] },
        });
        await workflow.submit();
        expect(workflow.phase).toBe('warnings');
        expect(workflow.target).not.toBeNull();
        expect(workflow.editable).toBe(false);
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledOnce();
        workflow.close();
        expect(workflow.target).toBeNull();
    });
    it('reviews a dropped hierarchy before writing, preserving the explicit target and empty directories', async () => {
        const { workflow, imports, mutations } = setup();
        const local = { name: 'raw', type: '', size: 42, readChunk: vi.fn() };
        imports.upload.mockResolvedValue([source]);
        await workflow.chooseDropped(async () => [
            { directory: true, relativePath: ['TREE'] },
            { directory: true, relativePath: ['TREE', 'EMPTY'] },
            { directory: false, relativePath: ['TREE', 'raw'], source: local },
        ]);
        expect(workflow.directoryMode).toBe(true);
        expect(imports.upload).toHaveBeenCalledWith([local], expect.any(AbortSignal), expect.any(Function));
        expect(mutations.execute).not.toHaveBeenCalled();
        expect(workflow.canSubmit).toBe(true);
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledWith(
            3,
            [
                { kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['TREE'] },
                { kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['TREE', 'EMPTY'] },
                {
                    kind: 'PUT_FILE',
                    parentEntryId: 'folder',
                    relativePath: ['TREE', 'raw'],
                    source,
                    expectedSource: snapshot,
                    conflict: 'SKIP',
                },
            ],
            expect.any(Function),
        );
    });
    it('rejects a dropped directory without directory capability and does not upload a partial failed scan', async () => {
        const { workflow, imports } = setup({ ...writableFilesRoot, createDirectory: false });
        await workflow.chooseDropped(async () => [{ directory: true, relativePath: ['EMPTY'] }]);
        expect(workflow.phase).toBe('failed');
        expect(imports.upload).not.toHaveBeenCalled();
        await workflow.chooseDropped(async () => {
            throw new Error('Read denied');
        });
        expect(workflow.message).toContain('Read denied');
        expect(imports.inspectInputs).not.toHaveBeenCalled();
    });
    it('cancels drop scanning and releases uploads that arrive after the review is closed', async () => {
        const { workflow, imports } = setup();
        let finish!: (value: (typeof source)[]) => void;
        imports.upload.mockImplementation(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const pending = workflow.chooseDropped(async () => [
            { directory: false, relativePath: ['RAW'], source: { name: 'RAW', size: 0, type: '', readChunk: vi.fn() } },
        ]);
        await vi.waitFor(() => expect(imports.upload).toHaveBeenCalledOnce());
        workflow.close();
        finish([source]);
        await pending;
        expect(imports.release).toHaveBeenCalledWith([source]);
        expect(imports.inspectInputs).not.toHaveBeenCalled();
        expect(workflow.target).toBeNull();
    });
    it('requires reviewed FAT 8.3 names without changing the chosen source identity', async () => {
        const { workflow, imports, mutations } = setup({
            ...writableFilesRoot,
            maximumNameBytes: 12,
            namePattern: "^[A-Z0-9!#$%&'()@^_`{}~-]{1,8}(\\.[A-Z0-9!#$%&'()@^_`{}~-]{1,3})?$",
            nameHint: 'Use uppercase ASCII 8.3 names.',
        });
        await workflow.chooseWorkspace();
        expect(workflow.rows[0].name).toBe('source.bin');
        expect(workflow.canInspect).toBe(false);
        expect(workflow.canSubmit).toBe(false);
        expect(imports.inspectDestination).not.toHaveBeenCalled();
        workflow.rename(0, 'TOO-LONG-NAME.BIN');
        expect(workflow.canInspect).toBe(false);
        workflow.rename(0, 'SOURCE.BIN');
        expect(workflow.canInspect).toBe(true);
        await workflow.inspect();
        expect(workflow.canSubmit).toBe(true);
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledWith(
            3,
            [
                {
                    kind: 'PUT_FILE',
                    parentEntryId: 'folder',
                    relativePath: ['SOURCE.BIN'],
                    source,
                    expectedSource: snapshot,
                    conflict: 'SKIP',
                },
            ],
            expect.any(Function),
        );
    });
    it('does not submit an all-merge directory review or accept a file decision for a directory', async () => {
        const { workflow, imports, mutations } = setup();
        imports.chooseDirectory.mockResolvedValue([{ relativePath: ['empty'], directory: true }]);
        imports.inspectDestination.mockImplementation(async (revision, parentEntryId, entries) => ({
            ...completed,
            result: {
                revision,
                parentEntryId,
                entries: entries.map((entry) => ({
                    ...entry,
                    action: 'MERGE_DIRECTORY',
                    existingSizeBytes: null,
                    issue: '',
                })),
            },
        }));
        await workflow.chooseDirectory();
        expect(workflow.phase).toBe('ready');
        expect(workflow.canSubmit).toBe(false);
        await workflow.submit();
        expect(mutations.execute).not.toHaveBeenCalled();
        imports.inspectDestination.mockImplementation(async (revision, parentEntryId, entries) => ({
            ...completed,
            result: {
                revision,
                parentEntryId,
                entries: entries.map((entry) => ({
                    ...entry,
                    action: 'CREATE_FILE',
                    existingSizeBytes: null,
                    issue: '',
                })),
            },
        }));
        await workflow.inspect();
        expect(workflow.phase).toBe('failed');
        expect(workflow.message).toContain('does not match');
    });
    it('aborts directory enumeration when closing and ignores its late result', async () => {
        const { workflow, imports } = setup();
        let resolve!: (entries: Awaited<ReturnType<FilesystemImportActions['chooseDirectory']>>) => void;
        imports.chooseDirectory.mockImplementation((signal, progress) => {
            progress('Reading source directory');
            return new Promise((done) => {
                resolve = done;
            });
        });
        const pending = workflow.chooseDirectory();
        expect(workflow.phase).toBe('scanning');
        workflow.close();
        expect(imports.chooseDirectory.mock.calls[0][0].aborted).toBe(true);
        resolve([{ relativePath: ['empty'], directory: true }]);
        await pending;
        expect(workflow.target).toBeNull();
        expect(imports.inspectDestination).not.toHaveBeenCalled();
    });
    it('reviews directory contents and submits parent-before-child edits with snapshots only for files', async () => {
        const { workflow, imports, mutations } = setup();
        imports.chooseDirectory.mockResolvedValue([
            { relativePath: ['folder'], directory: true },
            { relativePath: ['empty'], directory: true },
            { relativePath: ['folder', 'tone'], directory: false, source },
        ]);
        await workflow.chooseDirectory();
        expect(workflow.phase).toBe('ready');
        expect(imports.inspectInputs).toHaveBeenCalledWith([source], expect.any(Function));
        workflow.rename(0, 'renamed');
        await workflow.inspect();
        expect(imports.inspectDestination).toHaveBeenLastCalledWith(
            3,
            'folder',
            [
                { relativePath: ['renamed'], directory: true, sizeBytes: 0, conflict: 'SKIP' },
                { relativePath: ['empty'], directory: true, sizeBytes: 0, conflict: 'SKIP' },
                { relativePath: ['renamed', 'tone'], directory: false, sizeBytes: 42, conflict: 'SKIP' },
            ],
            expect.any(Function),
        );
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledWith(
            3,
            [
                { kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['renamed'] },
                { kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['empty'] },
                {
                    kind: 'PUT_FILE',
                    parentEntryId: 'folder',
                    relativePath: ['renamed', 'tone'],
                    source,
                    expectedSource: snapshot,
                    conflict: 'SKIP',
                },
            ],
            expect.any(Function),
        );
    });
    it('imports empty directories without requiring a file source inspection', async () => {
        const { workflow, imports } = setup();
        imports.chooseDirectory.mockResolvedValue([{ relativePath: ['empty'], directory: true }]);
        await workflow.chooseDirectory();
        expect(workflow.canSubmit).toBe(true);
        expect(imports.inspectInputs).not.toHaveBeenCalled();
        workflow.setAllConflicts('REPLACE');
        expect(workflow.rows[0].conflict).toBe('SKIP');
    });
    it('does not couple descendants when two source directories are renamed to the same destination', async () => {
        const { workflow, imports } = setup();
        imports.chooseDirectory.mockResolvedValue([
            { relativePath: ['a'], directory: true },
            { relativePath: ['b'], directory: true },
            { relativePath: ['a', 'aa'], directory: true },
            { relativePath: ['b', 'bb'], directory: true },
        ]);
        await workflow.chooseDirectory();
        workflow.rename(0, 'b');
        workflow.rename(1, 'c');
        await workflow.inspect();
        const entries = imports.inspectDestination.mock.lastCall![2];
        expect(entries.map((entry) => entry.relativePath)).toEqual([['b'], ['c'], ['b', 'aa'], ['c', 'bb']]);
    });
    it('can close and dispose without opening a review or releasing twice', async () => {
        const unopened = new FilesImportWorkflow();
        unopened.close();
        unopened.dispose();
        const { workflow, imports } = setup();
        workflow.close();
        workflow.close();
        workflow.dispose();
        await Promise.resolve();
        expect(imports.release).toHaveBeenCalledOnce();
    });
    it('reviews exact sources and destination before submitting one atomic batch', async () => {
        const { workflow, imports, mutations } = setup();
        await workflow.chooseWorkspace();
        expect(workflow.phase).toBe('ready');
        expect(imports.inspectInputs).toHaveBeenCalledWith([source], expect.any(Function));
        expect(imports.inspectDestination).toHaveBeenCalledWith(
            3,
            'folder',
            [{ relativePath: ['source.bin'], directory: false, sizeBytes: 42, conflict: 'SKIP' }],
            expect.any(Function),
        );
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledWith(
            3,
            [
                {
                    kind: 'PUT_FILE',
                    parentEntryId: 'folder',
                    relativePath: ['source.bin'],
                    source,
                    expectedSource: snapshot,
                    conflict: 'SKIP',
                },
            ],
            expect.any(Function),
        );
        expect(mutations.refresh).toHaveBeenCalledOnce();
        expect(workflow.target).toBeNull();
        expect(imports.release).toHaveBeenCalledWith([source]);
    });

    it('invalidates review after editing a name or conflict without silently changing its source snapshot', async () => {
        const { workflow, imports, mutations } = setup();
        await workflow.chooseWorkspace();
        workflow.rename(0, 'renamed.bin');
        workflow.setConflict(0, 'REPLACE');
        expect(workflow.canSubmit).toBe(false);
        await workflow.submit();
        expect(mutations.execute).not.toHaveBeenCalled();
        await workflow.inspect();
        expect(imports.inspectInputs).toHaveBeenCalledOnce();
        await workflow.submit();
        expect(mutations.execute.mock.calls[0][1][0]).toMatchObject({
            relativePath: ['renamed.bin'],
            conflict: 'REPLACE',
            expectedSource: snapshot,
        });
    });

    it('keeps invalid names editable and does not send them for review', async () => {
        const { workflow, imports } = setup();
        await workflow.chooseWorkspace();
        workflow.rename(0, '../outside');
        expect(workflow.canInspect).toBe(false);
        await workflow.inspect();
        expect(imports.inspectDestination).toHaveBeenCalledOnce();
    });

    it('does not submit an all-skipped or blocking review', async () => {
        const { workflow, imports, mutations } = setup();
        for (const action of ['SKIP_FILE', 'CONFLICT']) {
            imports.inspectDestination.mockImplementationOnce(async (revision, parentEntryId, entries) => ({
                ...completed,
                result: {
                    imageId: 'image',
                    revision,
                    parentEntryId,
                    conflictCount: action === 'CONFLICT' ? 1 : 0,
                    entries: entries.map((entry) => ({
                        ...entry,
                        action,
                        existingSizeBytes: 8,
                        issue: 'Existing entry',
                    })),
                },
            }));
            if (!workflow.rows.length) await workflow.chooseWorkspace();
            else await workflow.inspect();
            expect(workflow.canSubmit).toBe(false);
            await workflow.submit();
        }
        expect(mutations.execute).not.toHaveBeenCalled();
    });

    it('rejects missing, reordered or mismatched inspection data instead of pairing the wrong source', async () => {
        const { workflow, imports } = setup();
        imports.inspectInputs.mockResolvedValueOnce({
            ...completed,
            result: { inputs: [{ source: { fileRef: { rootId: 'other', relativePath: 'other' } }, snapshot }] },
        });
        await workflow.chooseWorkspace();
        expect(workflow.phase).toBe('failed');
        expect(imports.inspectDestination).not.toHaveBeenCalled();
    });

    it('rejects destination review with a different revision or path', async () => {
        const { workflow, imports } = setup();
        imports.inspectDestination.mockResolvedValueOnce({
            ...completed,
            result: { imageId: 'image', revision: 4, parentEntryId: 'folder', entries: [], conflictCount: 0 },
        });
        await workflow.chooseWorkspace();
        expect(workflow.phase).toBe('failed');
        expect(workflow.canSubmit).toBe(false);
    });

    it('ignores a late input selection after closing and releases its sources', async () => {
        const { workflow, imports } = setup();
        let finish!: (sources: (typeof source)[]) => void;
        imports.chooseFiles.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const pending = workflow.chooseWorkspace();
        workflow.close();
        finish([source]);
        await pending;
        expect(workflow.target).toBeNull();
        expect(imports.inspectInputs).not.toHaveBeenCalled();
        expect(imports.release).toHaveBeenCalledWith([source]);
    });

    it('never resubmits after an unconfirmed write outcome', async () => {
        const { workflow, mutations } = setup();
        await workflow.chooseWorkspace();
        mutations.execute.mockImplementationOnce(async (_revision, _edits, update) => {
            update({ ...completed, status: 'running' });
            throw new Error('Connection lost');
        });
        await workflow.submit();
        expect(workflow.phase).toBe('unconfirmed');
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledOnce();
        expect(mutations.observe).toHaveBeenCalledWith(9, expect.any(Function));
        expect(workflow.target).toBeNull();
    });

    it('only refreshes after committed changes when refreshing initially fails', async () => {
        const { workflow, mutations } = setup();
        await workflow.chooseWorkspace();
        mutations.refresh.mockRejectedValueOnce(new Error('offline'));
        await workflow.submit();
        expect(workflow.phase).toBe('refresh-failed');
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledOnce();
        expect(mutations.refresh).toHaveBeenCalledTimes(2);
    });

    it('requires refresh after a failed mutation rather than resubmitting the stale review', async () => {
        const { workflow, mutations } = setup();
        await workflow.chooseWorkspace();
        mutations.execute.mockResolvedValueOnce({ ...completed, status: 'failed', error: 'Image revision changed' });
        await workflow.submit();
        expect(workflow.phase).toBe('write-failed');
        expect(workflow.editable).toBe(false);
        await workflow.submit();
        expect(mutations.execute).toHaveBeenCalledOnce();
        expect(mutations.refresh).toHaveBeenCalledOnce();
        expect(workflow.target).toBeNull();
    });

    it('waits for cancellation acknowledgement and refreshes when completion wins the race', async () => {
        const { workflow, mutations, imports } = setup();
        await workflow.chooseWorkspace();
        let finish!: (job: JobState) => void;
        mutations.execute.mockImplementationOnce(async (_revision, _edits, update) => {
            update({ ...completed, status: 'running' });
            return new Promise((resolve) => {
                finish = resolve;
            });
        });
        const pending = workflow.submit();
        workflow.close();
        expect(workflow.target).not.toBeNull();
        await workflow.cancel();
        expect(mutations.cancel).toHaveBeenCalledWith(9);
        expect(imports.release).not.toHaveBeenCalledWith([source]);
        finish(completed);
        await pending;
        expect(mutations.refresh).toHaveBeenCalledOnce();
        expect(imports.release).toHaveBeenCalledWith([source]);
    });

    it('protects files, metadata and readonly targets before any source acquisition', () => {
        const { workflow, imports, mutations } = setup();
        workflow.close();
        for (const target of [
            filesystemEntry({ kind: 'file' }),
            filesystemEntry({ filesystemMetadata: true }),
            filesystemEntry({ issue: 'Missing' }),
        ])
            expect(workflow.open(3, target, writableFilesRoot, imports, mutations)).toBe(false);
        expect(workflow.open(3, filesystemEntry(), { ...writableFilesRoot, putFile: false }, imports, mutations)).toBe(
            false,
        );
    });
});
