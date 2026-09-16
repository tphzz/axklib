import { describe, expect, it, vi } from 'vitest';
import { FloppyImportWorkflow } from './floppyWorkflow.svelte';
import { floppySelection, floppyVolumeName } from './floppySelection';
import type { FloppyInspection } from '../../lib/floppyImport';
import { clientUploadLocation, serverFileLocation, serverDirectoryLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export const inspection: FloppyInspection = {
    format: 'A_SERIES',
    inspectionToken: 'inspection',
    complete: true,
    label: 'Source',
    nextRequiredIndex: null,
    members: [{ index: 1, label: 'Source' }],
    excludedFiles: [{ memberName: 'disk.img', path: 'SYSTEM2.002', sizeBytes: 100 }],
    issues: [],
    objects: [
        {
            objectKey: 'sample',
            name: 'Sample',
            displayName: 'Sample',
            objectType: 'SBNK',
            sizeBytes: 800,
            requiredObjectKeys: ['wave'],
            exclusionReason: '',
        },
        {
            objectKey: 'wave',
            name: 'Wave',
            displayName: 'Wave',
            objectType: 'SMPL',
            sizeBytes: 2000,
            requiredObjectKeys: [],
            exclusionReason: '',
        },
        {
            objectKey: 'bad',
            name: 'Unsupported',
            displayName: 'Unsupported',
            objectType: 'SMPL',
            sizeBytes: 100,
            requiredObjectKeys: [],
            exclusionReason: 'Unsupported profile',
        },
    ],
};
const plan: ImageSessionPackageImportPlan = {
    schemaVersion: '1.0',
    imageId: 'image',
    revision: 1,
    planToken: 'plan',
    expiresInSeconds: 900,
    planId: 'p',
    targetKind: 'SFS',
    targetSnapshotId: 'hash',
    valid: true,
    warnings: [],
    conflicts: [],
    actions: [],
    opaqueSequences: [],
    programAssignmentAdjustments: [],
    programSlotPlacements: [],
    allocation: [],
    sfsIndexCapacity: [],
    packages: [],
};
const source = serverFileLocation({ rootId: 'workspace', relativePath: 'disk.img' });
const volume: DiskTreeItem = { id: 'volume', name: 'Existing', kind: 'volume', partitionIndex: 0, childCount: 0 };
const partition: DiskTreeItem = {
    id: 'partition',
    name: 'Partition',
    kind: 'partition',
    partitionIndex: 0,
    childCount: 1,
    children: [volume],
};
function setup(value = inspection, isDesktop = false, connectionMode: ImageTransport['connectionMode'] = 'remote') {
    const transport = {
        connectionMode,
        startFloppyInspection: vi
            .fn()
            .mockResolvedValue({ jobId: 1, kind: 'images.floppy_import.inspect', status: 'completed', result: value }),
        releaseFloppyInspection: vi.fn().mockResolvedValue(undefined),
        planFloppyImport: vi.fn().mockResolvedValue(plan),
        releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
        startFloppyImport: vi
            .fn()
            .mockResolvedValue({ jobId: 2, kind: 'images.floppy_import', status: 'completed', result: {} }),
        uploadClientFile: vi
            .fn()
            .mockResolvedValue(clientUploadLocation({ uploadId: 'upload' }, 'DISK_IMAGE', 'disk.img')),
        releaseClientUpload: vi.fn().mockResolvedValue(undefined),
        cancelJob: vi.fn().mockResolvedValue(undefined),
        waitForJob: vi.fn(),
    };
    const refresh = vi.fn().mockResolvedValue(undefined),
        otherFormat = vi.fn().mockResolvedValue(undefined);
    const picker = new PickerController(() => undefined);
    const workflow = new FloppyImportWorkflow({
        transport: transport as unknown as ImageTransport,
        jobs: { run: (start: () => Promise<unknown>) => start() } as unknown as JobController,
        picker,
        isDesktop,
        sessionId: () => 1,
        sourceItems: () => [partition],
        mutationsAvailable: () => true,
        invalidateSession: vi.fn(),
        refreshSession: refresh,
        setStatus: vi.fn(),
        otherFormat,
    });
    return { workflow, transport, refresh, otherFormat, picker };
}
describe('floppy import', () => {
    it('suggests the first selected folder basename instead of its display path', async () => {
        const { workflow } = setup({ ...inspection, label: '   ' });
        const folder = serverDirectoryLocation(
            { rootId: 'workspace', relativePath: 'Drum Kits/norddrms/disk1/' },
            'Yamaha/axk/floppy/unpacked/Drum Kits/norddrms/disk1',
        );
        await workflow.requestDroppedFiles([folder], partition);
        expect(workflow.request?.volumeName).toBe('disk1');
        expect(workflow.request?.members[0].name).toBe(folder.displayName);
    });
    it('updates automatic suggestions when the first selected image changes', async () => {
        const { workflow } = setup({ ...inspection, label: '' });
        const first = serverFileLocation({ rootId: 'workspace', relativePath: 'images/second.IMG' });
        const second = serverFileLocation({ rootId: 'workspace', relativePath: 'images/first.ima' });
        await workflow.requestDroppedFiles([first, second], partition);
        expect(workflow.request?.volumeName).toBe('second');
        await workflow.remove(workflow.request!.members[0].id);
        expect(workflow.request?.volumeName).toBe('first');
    });
    it.each(['Custom', '', 'disk'])(
        'preserves edited new-volume draft %j across companions and modes',
        async (name) => {
            const { workflow } = setup({ ...inspection, label: '' });
            await workflow.requestDroppedFiles([source], partition);
            workflow.setDestination('create', 0, name);
            workflow.setMode('existing');
            workflow.setDestination('existing', 0, 'Existing');
            await workflow.add([serverFileLocation({ rootId: 'workspace', relativePath: 'disk2.img' })]);
            workflow.setMode('create');
            expect(workflow.request?.volumeName).toBe(name);
            await workflow.remove(workflow.request!.members[0].id);
            expect(workflow.request?.volumeName).toBe(name);
            await workflow.close();
            await workflow.requestDroppedFiles([source], partition);
            expect(workflow.request?.volumeName).toBe('disk');
        },
    );
    it('keeps partition changes automatic and adopts a subsequently available stored name', async () => {
        const { workflow, transport } = setup({ ...inspection, label: '', complete: false });
        await workflow.requestDroppedFiles([source], partition);
        workflow.setPartition(1);
        transport.startFloppyInspection.mockResolvedValue({
            jobId: 1,
            kind: 'images.floppy_import.inspect',
            status: 'completed',
            result: inspection,
        });
        await workflow.add([serverFileLocation({ rootId: 'workspace', relativePath: 'disk2.img' })]);
        expect(workflow.request).toMatchObject({ volumeName: 'Source', partitionIndex: 1 });
    });
    it('uses client filenames and maintains suggestions while Existing is selected', async () => {
        const { workflow } = setup({ ...inspection, label: '' });
        await workflow.requestDroppedFiles(
            [
                {
                    name: ['C:', 'images', 'Client.IMA'].join('\\'),
                    size: 512,
                    type: 'application/octet-stream',
                    readChunk: vi.fn(),
                },
            ],
            volume,
        );
        expect(workflow.request?.volumeName).toBe('Existing');
        workflow.setMode('create');
        expect(workflow.request?.volumeName).toBe('Client');
    });
    it('inspects a folder without uploading it and rejects mixed representations', async () => {
        const { workflow, picker, transport } = setup(inspection, true, 'local');
        const folder = serverDirectoryLocation({ rootId: 'workspace', relativePath: 'norddrms' });
        vi.spyOn(picker, 'chooseFloppySources').mockResolvedValue(folder);
        await workflow.chooseFiles(partition);
        expect(transport.startFloppyInspection).toHaveBeenCalledWith([folder]);
        expect(transport.uploadClientFile).not.toHaveBeenCalled();
        await workflow.add([source]);
        expect(workflow.request?.error).toContain('either disk images or unpacked disk folders');
        expect(workflow.request?.members).toHaveLength(1);
        await workflow.review();
        await workflow.apply();
        expect(workflow.request).toBeNull();
    });
    it('opens the storage picker directly for the bundled server and preserves the target', async () => {
        const { workflow, picker } = setup(inspection, true, 'local');
        const choose = vi.spyOn(picker, 'chooseFloppySources').mockResolvedValue([source]);
        await workflow.chooseFiles(volume);
        expect(choose).toHaveBeenCalledWith({
            parentDialog: 'floppy-import',
        });
        expect(workflow.request).toMatchObject({ volumeName: 'Existing', mode: 'existing' });
        expect(workflow.request?.inspection?.complete).toBe(true);
    });
    it('closes an empty direct import when its picker is cancelled', async () => {
        const { workflow, picker } = setup(inspection, true, 'local');
        vi.spyOn(picker, 'chooseFloppySources').mockResolvedValue(null);
        await workflow.chooseFiles(volume);
        expect(workflow.request).toBeNull();
    });
    it('preserves selected disks when adding a companion is cancelled', async () => {
        const { workflow, picker } = setup(inspection, true, 'local');
        await workflow.requestDroppedFiles([source], volume);
        vi.spyOn(picker, 'chooseFloppySources').mockResolvedValue(null);
        await workflow.chooseWorkspace();
        expect(workflow.request?.members).toHaveLength(1);
        expect(workflow.request?.inspection?.complete).toBe(true);
    });
    it('retains direct routing after picker failure', async () => {
        const { workflow, picker } = setup(inspection, true, 'local');
        vi.spyOn(picker, 'chooseFloppySources').mockRejectedValue(new Error('Picker failed'));
        await workflow.chooseFiles(volume);
        expect(workflow.request?.error).toBe('Picker failed');
        expect(workflow.directSource).toBe(true);
    });
    it('discards a picker result after the request is replaced', async () => {
        const { workflow, picker, transport } = setup(inspection, true, 'local');
        let finish!: (files: (typeof source)[]) => void;
        vi.spyOn(picker, 'chooseFloppySources').mockReturnValue(new Promise((resolve) => (finish = resolve)));
        const choosing = workflow.chooseFiles(volume);
        await workflow.close();
        workflow.open(partition);
        finish([source]);
        await choosing;
        expect(workflow.request?.members).toHaveLength(0);
        expect(workflow.request?.mode).toBe('create');
        expect(transport.startFloppyInspection).not.toHaveBeenCalled();
    });
    it.each([
        [true, 'remote'],
        [false, 'local'],
    ] as const)('keeps source choices for desktop=%s connection=%s', async (desktop, mode) => {
        const { workflow, picker } = setup(inspection, desktop, mode);
        const choose = vi.spyOn(picker, 'chooseFloppySources');
        await workflow.chooseFiles(volume);
        expect(choose).not.toHaveBeenCalled();
        expect(workflow.request?.status).toBe('choosing');
    });
    it('selects supported roots and locks their dependencies; configuration stays excluded', async () => {
        const { workflow } = setup();
        await workflow.requestDroppedFiles([source], volume);
        expect(workflow.request).toMatchObject({
            mode: 'existing',
            volumeName: 'Existing',
            selected: ['sample', 'wave'],
        });
        workflow.toggle('wave', false);
        expect(workflow.request?.selected).toContain('wave');
        workflow.selectAll(false);
        workflow.toggle('sample', true);
        expect([...workflow.selection().included]).toEqual(['sample', 'wave']);
        workflow.toggle('bad', true);
        expect(workflow.request?.selected).toEqual(['sample']);
        expect(workflow.request?.inspection?.excludedFiles[0].path).toBe('SYSTEM2.002');
    });
    it('captures a new-volume destination, reviews selected roots, and closes only after refresh', async () => {
        const { workflow, transport, refresh } = setup();
        await workflow.requestDroppedFiles([source], partition);
        expect(workflow.request).toMatchObject({ mode: 'create', volumeName: 'Source' });
        workflow.selectAll(false);
        workflow.toggle('sample', true);
        await workflow.review();
        expect(transport.planFloppyImport).toHaveBeenCalledWith(
            1,
            expect.objectContaining({
                selectedObjectKeys: ['sample'],
                destination: { kind: 'CREATE_VOLUME', partitionIndex: 0, volumeName: 'Source' },
            }),
        );
        let finish!: () => void;
        refresh.mockImplementationOnce(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        const applying = workflow.apply();
        await vi.waitFor(() => expect(refresh).toHaveBeenCalled());
        expect(workflow.request).not.toBeNull();
        expect(transport.releaseFloppyInspection).toHaveBeenCalledWith('inspection');
        finish();
        await applying;
        expect(workflow.request).toBeNull();
        expect(transport.startFloppyImport).toHaveBeenCalledTimes(1);
    });
    it('retains refresh failure recovery without repeating the write', async () => {
        const { workflow, transport, refresh } = setup();
        await workflow.requestDroppedFiles([source], volume);
        await workflow.review();
        refresh.mockRejectedValueOnce(new Error('refresh failed'));
        await workflow.apply();
        expect(workflow.completion.phase).toBe('refresh-failed');
        await workflow.apply();
        await workflow.recover();
        expect(transport.startFloppyImport).toHaveBeenCalledTimes(1);
        expect(refresh).toHaveBeenCalledTimes(2);
        expect(workflow.request).toBeNull();
    });
    it('blocks incomplete disk sets and forwards recognized TX16W content to its importer', async () => {
        const missing = setup({ ...inspection, complete: false, nextRequiredIndex: 2, objects: [] });
        await missing.workflow.requestDroppedFiles([source], volume);
        await missing.workflow.review();
        expect(missing.transport.planFloppyImport).not.toHaveBeenCalled();
        const other = setup({ ...inspection, format: 'TX16W', inspectionToken: null });
        await other.workflow.requestDroppedFiles([source], volume);
        expect(other.otherFormat).toHaveBeenCalledWith('TX16W', [source], volume);
        expect(other.workflow.request).toBeNull();
    });
    it('invalidates a reviewed plan after destination or selection edits', async () => {
        const { workflow, transport } = setup();
        await workflow.requestDroppedFiles([source], volume);
        await workflow.review();
        workflow.setDestination('create', 0, 'Another');
        await workflow.apply();
        expect(workflow.request?.plan).toBeNull();
        expect(transport.startFloppyImport).not.toHaveBeenCalled();
        expect(transport.releaseImagePackageImportPlan).toHaveBeenCalledWith('plan');
    });
    it('releases uploads that finish after cancellation without publishing an inspection', async () => {
        const { workflow, transport } = setup();
        let upload!: (value: unknown) => void;
        transport.uploadClientFile.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    upload = resolve;
                }),
        );
        const pending = workflow.requestDroppedFiles(
            [{ name: 'disk.img', size: 512, type: 'application/octet-stream', readChunk: vi.fn() }],
            volume,
        );
        await vi.waitFor(() => expect(transport.uploadClientFile).toHaveBeenCalled());
        await workflow.close();
        upload(clientUploadLocation({ uploadId: 'late' }, 'DISK_IMAGE', 'disk.img'));
        await pending;
        expect(transport.startFloppyInspection).not.toHaveBeenCalled();
        expect(transport.releaseClientUpload).toHaveBeenCalled();
    });
    it('bounds dependency cycles and uses a valid label or filename for volume names', () => {
        const objects = inspection.objects.map((o) =>
            o.objectKey === 'wave' ? { ...o, requiredObjectKeys: ['sample'] } : o,
        );
        expect(floppySelection(objects, ['sample']).included.size).toBe(2);
        expect(floppyVolumeName('', 'long floppy filename.img', 'file')).toBe('long floppy file');
        expect(floppyVolumeName(' VALID ', 'fallback.img', 'file')).toBe('VALID');
    });
});
