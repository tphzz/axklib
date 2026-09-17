import { describe, expect, it, vi } from 'vitest';
import { Su700Workflow } from './su700Workflow.svelte';
import type { AxklibFilesystemImports } from './su700Actions';
import { FilesController } from '../../files/controller.svelte';
import { filesystemEntry, writableFilesRoot } from '../../../lib/testing/filesystem';
import { serverFileLocation } from '../../../lib/storageLocations';
import type { Su700Inspection } from '../../../lib/su700Import';
import { isFloppyCandidate } from './su700Drop';

const inspection: Su700Inspection = {
    status: 'COMPLETE',
    issue: '',
    snapshot: { revision: 'source-1', sha256: 'a'.repeat(64), sizeBytes: 100 },
    suggestedVolumeName: 'DEMO',
    songCount: 1,
    sampleCount: 1,
    destinationReady: false,
    totalBytes: 100,
    files: [
        { sourcePath: 'SONGCONT.DAT', relativePath: ['SONGCONT.DAT'], sizeBytes: 7400, role: 'CONTROL' },
        { sourcePath: 'SONG.SSQ', relativePath: ['SUSQ', 'SONG    .SSQ'], sizeBytes: 16, role: 'SONG' },
        { sourcePath: 'README', relativePath: ['README'], sizeBytes: 3, role: 'EXTRA' },
    ],
};
async function setup() {
    const root = filesystemEntry({
        id: 'root',
        rootId: 'root',
        kind: 'root',
        name: 'SU700',
        parentId: null,
        ancestorIds: [],
    });
    const controller = new FilesController({
        inspect: async (query = {}) => ({
            revision: 2,
            available: true,
            filesystemName: 'SFS',
            deviceView: null,
            items: query.parentId ? [] : [root],
            totalCount: query.parentId ? 0 : 1,
            rootCapabilities: [{ ...writableFilesRoot, rootId: 'root', supportedImports: ['SU700_FLOPPY'] }],
        }),
    });
    await controller.initialize();
    const run = vi
        .fn<NonNullable<AxklibFilesystemImports['su700']>['run']>()
        .mockImplementation(async (request, update) => {
            const job = {
                jobId: 4,
                kind: 'images.su700.import',
                status: 'completed' as const,
                result: request.expectedSource
                    ? { imageId: 'image', revision: 3 }
                    : { ...inspection, destinationReady: !!request.destination },
            };
            update(job);
            return job;
        });
    const imports: AxklibFilesystemImports = {
        supportsClientUploads: true,
        chooseFiles: vi.fn().mockResolvedValue([serverFileLocation({ rootId: 'source', relativePath: 'floppy.img' })]),
        chooseDirectory: vi.fn(),
        upload: vi.fn(),
        release: vi.fn().mockResolvedValue(undefined),
        inspectInputs: vi.fn(),
        inspectDestination: vi.fn(),
        observe: vi.fn(),
        cancel: vi.fn(),
        su700: { run, observe: vi.fn(), cancel: vi.fn().mockResolvedValue(undefined) },
    };
    const refresh = vi.fn().mockResolvedValue(undefined);
    return { workflow: new Su700Workflow(() => controller, imports, 7, refresh), imports, run, refresh, controller };
}
describe('SU700 import review', () => {
    it('does not dismiss or cancel while refreshing a committed import', async () => {
        const { workflow, refresh, imports } = await setup();
        await workflow.open();
        await workflow.review();
        let finish!: () => void;
        refresh.mockImplementationOnce(() => new Promise<void>((resolve) => (finish = resolve)));
        const pending = workflow.submit();
        await vi.waitFor(() => expect(refresh).toHaveBeenCalledOnce());
        expect(workflow.canDismiss).toBe(false);
        await workflow.close();
        expect(imports.su700!.cancel).not.toHaveBeenCalled();
        expect(workflow.opened).toBe(true);
        finish();
        await pending;
        expect(workflow.opened).toBe(false);
    });
    it('retains completion warnings without allowing another import', async () => {
        const { workflow, run } = await setup();
        await workflow.open();
        await workflow.review();
        run.mockResolvedValueOnce({
            jobId: 4,
            kind: 'review',
            status: 'completed',
            result: { ...inspection, destinationReady: true },
        });
        run.mockResolvedValueOnce({
            jobId: 5,
            kind: 'import',
            status: 'completed',
            result: { imageId: 'image', revision: 3, warnings: ['Check the imported song'] },
        });
        await workflow.submit();
        expect(workflow.opened).toBe(true);
        expect(workflow.phase).toBe('warnings');
        expect(workflow.canSubmit).toBe(false);
        expect(workflow.warnings).toEqual(['Check the imported song']);
        await workflow.close();
        expect(workflow.opened).toBe(false);
    });
    it('requires review, captures revision, defaults extras on, and commits once with source guards', async () => {
        const { workflow, run, refresh, controller } = await setup();
        await workflow.open();
        expect(workflow.extras).toEqual(['README']);
        expect(workflow.canSubmit).toBe(false);
        workflow.updateName('New Volume');
        await workflow.review();
        expect(workflow.canSubmit).toBe(true);
        workflow.toggleExtra('README');
        expect(workflow.canSubmit).toBe(false);
        await workflow.review();
        await workflow.submit();
        await workflow.submit();
        const mutations = run.mock.calls.filter(([request]) => request.expectedSource);
        expect(mutations).toHaveLength(1);
        expect(mutations[0][0]).toMatchObject({
            destination: { sessionId: 7, expectedRevision: 2, rootEntryId: 'root', volumeName: 'New Volume' },
            includedExtras: [],
            expectedSource: inspection.snapshot,
        });
        expect(workflow.phase).toBe('complete');
        expect(workflow.opened).toBe(false);
        expect(refresh).toHaveBeenCalledOnce();
        expect(controller.selection).toEqual([]);
    });
    it.each(['../bad', ' bad', 'bad ', '', 'x'.repeat(17), 'non-ascii-\u00e9'])(
        'rejects invalid volume name %s',
        async (name) => {
            const { workflow } = await setup();
            await workflow.open();
            workflow.updateName(name);
            expect(workflow.canReview).toBe(false);
        },
    );
    it('requires confirmation again when source changes during final reinspection', async () => {
        const { workflow, run } = await setup();
        await workflow.open();
        await workflow.review();
        run.mockResolvedValueOnce({
            jobId: 5,
            kind: 'inspect',
            status: 'completed',
            result: {
                ...inspection,
                destinationReady: true,
                snapshot: { ...inspection.snapshot, sha256: 'b'.repeat(64) },
            },
        });
        await workflow.submit();
        expect(workflow.message).toContain('Source contents changed');
        expect(workflow.canSubmit).toBe(false);
        expect(run.mock.calls.some(([request]) => request.expectedSource)).toBe(false);
    });
    it('blocks malformed or incomplete floppies without a write', async () => {
        const { workflow, run } = await setup();
        run.mockResolvedValueOnce({
            jobId: 1,
            kind: 'inspect',
            status: 'completed',
            result: { ...inspection, status: 'UNSUPPORTED', issue: 'Missing sample', files: [] },
        });
        await workflow.open();
        expect(workflow.message).toBe('Missing sample');
        expect(workflow.canReview).toBe(false);
        await workflow.submit();
        expect(run).toHaveBeenCalledOnce();
    });
    it('does not resubmit after a lost write response and recovers by observing the same job', async () => {
        const { workflow, run, imports, refresh } = await setup();
        await workflow.open();
        await workflow.review();
        run.mockImplementation(async (request, update) => {
            if (!request.expectedSource)
                return {
                    jobId: 3,
                    kind: 'inspect',
                    status: 'completed',
                    result: { ...inspection, destinationReady: true },
                };
            update({ jobId: 9, kind: 'import', status: 'running' });
            throw Error('Connection interrupted');
        });
        await workflow.submit();
        expect(workflow.phase).toBe('unconfirmed');
        await workflow.close();
        expect(workflow.opened).toBe(true);
        await workflow.submit();
        expect(run.mock.calls.filter(([r]) => r.expectedSource)).toHaveLength(1);
        vi.mocked(imports.su700!.observe).mockResolvedValueOnce({
            jobId: 9,
            kind: 'import',
            status: 'completed',
            result: {},
        });
        await workflow.checkStatus();
        expect(refresh).toHaveBeenCalledOnce();
        expect(workflow.phase).toBe('complete');
    });
    it('separates a committed write from a refresh failure', async () => {
        const { workflow, refresh, run } = await setup();
        await workflow.open();
        await workflow.review();
        refresh.mockRejectedValueOnce(Error('offline'));
        await workflow.submit();
        expect(workflow.phase).toBe('refresh-failed');
        await workflow.retryRefresh();
        expect(workflow.phase).toBe('complete');
        expect(run.mock.calls.filter(([r]) => r.expectedSource)).toHaveLength(1);
    });
    it('recovers an unknown submission using the same request and idempotency key', async () => {
        const { workflow, run } = await setup();
        await workflow.open();
        await workflow.review();
        let rejected = false;
        run.mockImplementation(async (request) => {
            if (!request.expectedSource)
                return {
                    jobId: 3,
                    kind: 'inspect',
                    status: 'completed',
                    result: { ...inspection, destinationReady: true },
                };
            if (!rejected) {
                rejected = true;
                throw Error('Response lost');
            }
            return { jobId: 9, kind: 'import', status: 'completed', result: {} };
        });
        await workflow.submit();
        expect(workflow.phase).toBe('unconfirmed');
        await workflow.checkStatus();
        expect(workflow.phase).toBe('complete');
        const writes = run.mock.calls.filter(([r]) => r.expectedSource);
        expect(writes).toHaveLength(2);
        expect(writes[0][0]).toEqual(writes[1][0]);
        expect(writes[0][0].idempotencyKey).toBeTruthy();
    });
    it('cancels a write without releasing its source before the terminal outcome', async () => {
        const { workflow, run, imports } = await setup();
        await workflow.open();
        await workflow.review();
        let finish!: () => void;
        run.mockImplementation(async (request, update) => {
            if (!request.expectedSource)
                return {
                    jobId: 3,
                    kind: 'inspect',
                    status: 'completed',
                    result: { ...inspection, destinationReady: true },
                };
            update({ jobId: 9, kind: 'import', status: 'running' });
            await new Promise<void>((resolve) => (finish = resolve));
            return { jobId: 9, kind: 'import', status: 'cancelled' };
        });
        const pending = workflow.submit();
        await vi.waitFor(() => expect(workflow.phase).toBe('writing'));
        await workflow.close();
        expect(imports.su700!.cancel).toHaveBeenCalledWith(9);
        expect(imports.release).not.toHaveBeenCalled();
        finish();
        await pending;
        expect(imports.release).toHaveBeenCalledOnce();
        expect(workflow.canSubmit).toBe(false);
    });
    it('cancels an inspection and releases its source after it stops', async () => {
        const { workflow, run, imports } = await setup();
        let finish!: () => void;
        run.mockImplementationOnce(async (_, update) => {
            update({ jobId: 8, kind: 'inspect', status: 'running' });
            await new Promise<void>((resolve) => (finish = resolve));
            return { jobId: 8, kind: 'inspect', status: 'cancelled' };
        });
        const pending = workflow.open();
        await vi.waitFor(() => expect(run).toHaveBeenCalledOnce());
        await workflow.close();
        expect(imports.su700!.cancel).toHaveBeenCalledWith(8);
        finish();
        await pending;
        expect(workflow.opened).toBe(false);
        expect(imports.release).toHaveBeenCalledOnce();
    });
});
describe('Floppy drop admission', () => {
    it('uses the BPB instead of an extension, leaving ordinary files to the generic importer', async () => {
        const bytes = new Uint8Array(512);
        bytes[12] = 2;
        bytes[16] = 2;
        bytes[22] = 9;
        const source = { name: 'no-extension', type: '', size: 1474560, readChunk: async () => new Blob([bytes]) };
        const entry = { directory: false as const, relativePath: ['no-extension'], source };
        expect(await isFloppyCandidate([entry])).toBe(true);
        bytes[12] = 0;
        expect(await isFloppyCandidate([{ ...entry, relativePath: ['looks-like.img'] }])).toBe(false);
        expect(await isFloppyCandidate([entry, entry])).toBe(false);
    });
});
