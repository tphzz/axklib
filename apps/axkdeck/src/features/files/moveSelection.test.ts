import { expect, it, vi } from 'vitest';
import { moveConflicts, moveSelection, moveTargetAllowed } from './moveSelection';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import { FilesEditWorkflow } from './editWorkflow.svelte';

const folder = filesystemEntry();
const child = filesystemEntry({
    id: 'child',
    kind: 'file',
    name: 'A.BIN',
    path: '/Documents/A.BIN',
    parentId: folder.id,
    ancestorIds: ['root', folder.id],
});
const target = filesystemEntry({ id: 'target', name: 'Target', path: '/Target' });

it('normalizes selections and rejects cycles, other partitions, metadata and read-only sources', () => {
    expect(moveSelection([folder, child, folder], target)).toEqual([folder]);
    expect(moveSelection([child], folder)).toEqual([]);
    expect(moveTargetAllowed([folder, child], target)).toBe(true);
    for (const invalid of [folder, child, { ...target, rootId: 'other' }, { ...target, filesystemMetadata: true }])
        expect(moveTargetAllowed([folder], invalid)).toBe(false);
    expect(
        moveTargetAllowed(
            [{ ...child, attributes: [{ code: 'fat.read-only', label: '', value: '', description: '', summary: '' }] }],
            target,
        ),
    ).toBe(false);
});

it('blocks existing and in-selection names under the filesystem case policy', () => {
    const lower = { ...child, id: 'lower', name: 'a.bin' };
    expect(moveConflicts([child], [lower], writableFilesRoot)).toEqual({});
    const fat = { ...writableFilesRoot, namePolicy: 'FAT_8_3_UPPERCASE' as const };
    expect(Object.keys(moveConflicts([child], [lower], fat))).toEqual(['child']);
    expect(Object.keys(moveConflicts([child, lower], [], fat))).toEqual(['child', 'lower']);
});

it('confirms a normalized batch and only refreshes after a committed move', async () => {
    const moved = vi.fn();
    const workflow = new FilesEditWorkflow(undefined, undefined, moved);
    const driver = {
        execute: vi.fn().mockResolvedValue({ jobId: 5, status: 'completed', result: { revision: 4 } }),
        observe: vi.fn(),
        cancel: vi.fn(),
        refresh: vi.fn().mockRejectedValueOnce(new Error('offline')).mockResolvedValue(undefined),
    };
    await workflow.openMove(
        { kind: 'move', revision: 3, entries: [folder, child], destination: target, capabilities: writableFilesRoot },
        driver,
        async () => [],
    );
    expect(workflow.canSubmit).toBe(true);
    expect(driver.execute).not.toHaveBeenCalled();
    await workflow.submit();
    expect(driver.execute).toHaveBeenCalledWith(
        3,
        [{ kind: 'MOVE', entryId: folder.id, destinationParentEntryId: target.id }],
        expect.any(Function),
    );
    expect(moved).toHaveBeenCalledOnce();
    expect(workflow.phase).toBe('refresh-failed');
    expect(workflow.canClose).toBe(false);
    await workflow.submit();
    expect(driver.execute).toHaveBeenCalledOnce();
    expect(workflow.review).toBeNull();
});

it('blocks the entire conflicting batch and allows cancelling an unfinished review', async () => {
    const workflow = new FilesEditWorkflow();
    const driver = { execute: vi.fn(), observe: vi.fn(), cancel: vi.fn(), refresh: vi.fn() };
    await workflow.openMove(
        { kind: 'move', revision: 3, entries: [folder, child], destination: target, capabilities: writableFilesRoot },
        driver,
        async () => [folder],
    );
    expect(workflow.canSubmit).toBe(false);
    await workflow.submit();
    expect(driver.execute).not.toHaveBeenCalled();
    workflow.close();
    let complete!: (items: (typeof child)[]) => void;
    const pending = workflow.openMove(
        { kind: 'move', revision: 3, entries: [child], destination: target, capabilities: writableFilesRoot },
        driver,
        () =>
            new Promise((resolve) => {
                complete = resolve;
            }),
    );
    expect(workflow.canSubmit).toBe(false);
    workflow.close();
    complete([]);
    await pending;
    expect(workflow.review).toBeNull();
    expect(driver.execute).not.toHaveBeenCalled();
});
