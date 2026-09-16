import { describe, expect, it, vi } from 'vitest';
import type { FilesystemMutationDriver } from '../../lib/filesystem';
import { FilesystemWriteRejected } from '../../lib/filesystem';
import type { JobState } from '../../lib/transport';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import { FilesEditWorkflow, validFilesystemName } from './editWorkflow.svelte';

const completed: JobState = { jobId: 7, kind: 'images.filesystem.edit', status: 'completed' };
function setup(kind: 'create' | 'delete' = 'create') {
    const driver = {
        execute: vi.fn<FilesystemMutationDriver['execute']>().mockResolvedValue(completed),
        observe: vi.fn<FilesystemMutationDriver['observe']>().mockResolvedValue(completed),
        cancel: vi.fn<FilesystemMutationDriver['cancel']>().mockResolvedValue(),
        refresh: vi.fn<FilesystemMutationDriver['refresh']>().mockResolvedValue(),
    };
    const workflow = new FilesEditWorkflow();
    workflow.open({ kind, revision: 3, entries: [filesystemEntry()], capabilities: writableFilesRoot }, driver);
    workflow.name = 'New';
    return { workflow, driver };
}

describe('Files edits', () => {
    it('allows dismissal after a definite submission rejection without inferring an uncertain outcome', async () => {
        const { workflow, driver } = setup();
        driver.execute.mockRejectedValueOnce(new FilesystemWriteRejected('Revision changed'));
        await workflow.submit();
        expect(workflow.phase).toBe('failed');
        expect(workflow.canClose).toBe(true);
        workflow.close();
        expect(workflow.review).toBeNull();
        expect(driver.refresh).not.toHaveBeenCalled();
    });
    it('requires one writable rename target and retains a committed rename through refresh recovery', async () => {
        const { driver, workflow: unused } = setup();
        unused.close();
        const renamed = vi.fn();
        const status = vi.fn();
        const workflow = new FilesEditWorkflow(renamed, status);
        const review = {
            kind: 'rename' as const,
            revision: 3,
            entries: [
                filesystemEntry({
                    kind: 'file',
                    attributes: [
                        {
                            code: 'sfs.file-write',
                            label: 'File write flag',
                            value: 'Disabled',
                            summary: 'File write flag: Disabled',
                            description: '',
                        },
                    ],
                }),
            ],
            capabilities: writableFilesRoot,
        };
        for (const entry of [
            filesystemEntry({ parentId: null }),
            filesystemEntry({ filesystemMetadata: true }),
            filesystemEntry({ issue: 'Missing' }),
            filesystemEntry({
                attributes: [
                    {
                        code: 'fat.read-only',
                        label: 'Localized label',
                        value: 'Yes',
                        description: '',
                        summary: 'Localized label',
                    },
                ],
            }),
        ])
            expect(workflow.open({ ...review, entries: [entry] }, driver)).toBe(false);
        expect(workflow.open({ ...review, entries: [filesystemEntry(), filesystemEntry()] }, driver)).toBe(false);
        expect(workflow.open({ ...review, capabilities: { ...writableFilesRoot, renameEntry: false } }, driver)).toBe(
            false,
        );
        expect(workflow.open(review, driver)).toBe(true);
        expect(workflow.name).toBe('Documents');
        expect(workflow.canSubmit).toBe(false);
        workflow.name = 'Renamed';
        driver.execute.mockResolvedValue({ ...completed, result: { imageId: 'image', revision: 4, warnings: [] } });
        driver.refresh.mockRejectedValueOnce(new Error('offline'));
        await workflow.submit();
        expect(renamed).toHaveBeenCalledWith(review, 'Renamed', 4);
        expect(workflow.canClose).toBe(false);
        workflow.close();
        expect(workflow.review).not.toBeNull();
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(renamed).toHaveBeenCalledOnce();
        expect(status).toHaveBeenCalledWith('Renamed to Renamed');
        expect(workflow.review).toBeNull();
    });

    it('validates native naming rules in addition to path-component safety', () => {
        for (const name of ['', '.', '..', 'a/b', 'a\\b', 'a\0b', 'A'.repeat(24), '\u00e9', 'New\n', 'New\r\n'])
            expect(validFilesystemName(name, writableFilesRoot)).toBe(false);
        expect(validFilesystemName('New directory', writableFilesRoot)).toBe(true);
        expect(validFilesystemName('ok', { ...writableFilesRoot, namePattern: '[' })).toBe(false);
    });
    it('creates in the reviewed directory at its captured revision and refreshes', async () => {
        const { workflow, driver } = setup();
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledWith(
            3,
            [{ kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['New'] }],
            expect.any(Function),
        );
        expect(driver.refresh).toHaveBeenCalledOnce();
        expect(workflow.review).toBeNull();
    });
    it('never resubmits a committed write after refresh fails', async () => {
        const { workflow, driver } = setup();
        driver.refresh.mockRejectedValueOnce(new Error('offline'));
        await workflow.submit();
        expect(workflow.phase).toBe('refresh-failed');
        expect(workflow.message).toContain('Changes saved');
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(driver.refresh).toHaveBeenCalledTimes(2);
        expect(workflow.review).toBeNull();
    });
    it('observes a known job after interrupted status delivery rather than repeating it', async () => {
        const { workflow, driver } = setup();
        driver.execute.mockImplementationOnce(async (_revision, _edits, update) => {
            update({ ...completed, status: 'running' });
            throw new Error('connection lost');
        });
        await workflow.submit();
        expect(workflow.phase).toBe('unconfirmed');
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(driver.observe).toHaveBeenCalledWith(7, expect.any(Function));
        expect(workflow.review).toBeNull();
    });
    it('cannot infer an outcome from refresh when no job identity arrives', async () => {
        const { workflow, driver } = setup();
        driver.execute.mockRejectedValueOnce(new Error('connection lost'));
        await workflow.submit();
        expect(workflow.message).toContain('unconfirmed');
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(driver.refresh).not.toHaveBeenCalled();
        expect(workflow.canSubmit).toBe(false);
        workflow.close();
        expect(workflow.review).not.toBeNull();
    });
    it('confirms recursive deletion and removes overlapping descendants from the submitted batch', async () => {
        const { workflow, driver } = setup('delete');
        workflow.review!.entries.push(
            filesystemEntry({ id: 'child', kind: 'file', parentId: 'folder', ancestorIds: ['root', 'folder'] }),
        );
        await workflow.submit();
        expect(driver.execute).toHaveBeenCalledWith(
            3,
            [{ kind: 'DELETE', entryId: 'folder', recursive: true }],
            expect.any(Function),
        );
    });
    it('protects roots, metadata and foreign roots even when an action is invoked directly', () => {
        const { workflow, driver } = setup();
        workflow.close();
        for (const entry of [
            filesystemEntry({ parentId: null }),
            filesystemEntry({ filesystemMetadata: true }),
            filesystemEntry({ rootId: 'other' }),
            filesystemEntry({ issue: 'Missing' }),
        ]) {
            expect(
                workflow.open(
                    { kind: 'delete', revision: 3, entries: [entry], capabilities: writableFilesRoot },
                    driver,
                ),
            ).toBe(false);
        }
        expect(
            workflow.open(
                {
                    kind: 'create',
                    revision: 3,
                    entries: [filesystemEntry()],
                    capabilities: { ...writableFilesRoot, createDirectory: false },
                },
                driver,
            ),
        ).toBe(false);
    });
    it('keeps the dialog until cancellation is acknowledged and refreshes on a completed race', async () => {
        const { workflow, driver } = setup();
        let finish!: (job: JobState) => void;
        driver.execute.mockImplementationOnce(async (_revision, _edits, update) => {
            update({ ...completed, status: 'running' });
            return new Promise((resolve) => {
                finish = resolve;
            });
        });
        const running = workflow.submit();
        workflow.close();
        expect(workflow.review).not.toBeNull();
        await workflow.cancel();
        expect(driver.cancel).toHaveBeenCalledWith(7);
        finish(completed);
        await running;
        expect(driver.refresh).toHaveBeenCalledOnce();
        expect(workflow.review).toBeNull();
    });
});
