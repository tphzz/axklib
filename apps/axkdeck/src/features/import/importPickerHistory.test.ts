import { describe, expect, it } from 'vitest';
import { PickerController, type PickerRequest } from '../dialogs/picker';
import { JobController } from '../jobs/actions';
import type { FloppyInspection } from '../../lib/floppyImport';
import type { DirectoryRef } from '../../lib/storageLocations';
import { serverDirectoryLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { createObjectImports } from './createObjectImports';
import { PackagePickerHistory } from './packagePickerHistory';

const volume: DiskTreeItem = { id: 'v', name: 'Existing', kind: 'volume', partitionIndex: 0, childCount: 0 };
const partition: DiskTreeItem = {
    id: 'p',
    name: 'Partition',
    kind: 'partition',
    partitionIndex: 0,
    childCount: 1,
    children: [volume],
};
const disks: DirectoryRef = { rootId: 'workspace', relativePath: 'floppies/kit' };
const packages: DirectoryRef = { rootId: 'workspace', relativePath: 'packages' };

function setup(isDesktop = true) {
    const requests: PickerRequest[] = [];
    const picker = new PickerController((request) => {
        if (request) requests.push(request);
    });
    const inspection: FloppyInspection = {
        format: 'A_SERIES',
        inspectionToken: 'inspection',
        complete: false,
        canImport: false,
        recoveryUsed: false,
        requiresAcknowledgement: true,
        label: 'Kit',
        nextRequiredIndex: 2,
        members: [{ index: 1, label: 'Kit' }],
        excludedFiles: [],
        issues: [],
        objects: [],
    };
    const job = { jobId: 1, kind: 'images.floppy_import.inspect', status: 'completed' as const, result: inspection };
    const transport = {
        connectionMode: isDesktop ? 'local' : 'remote',
        startFloppyInspection: async () => job,
        waitForJob: async () => job,
        cancelJob: async () => {},
        releaseFloppyInspection: async () => {},
    } as unknown as ImageTransport;
    const imports = createObjectImports({
        transport,
        jobs: new JobController(transport),
        picker,
        pickerHistory: new PackagePickerHistory(),
        isDesktop,
        sessionId: () => 1,
        sourceItems: () => [partition],
        mutationsAvailable: () => true,
        invalidateSession: async () => {},
        refreshSession: async () => {},
        setStatus: () => {},
        otherFormat: async () => {},
    });
    return { ...imports, picker, requests };
}

describe('session-only import picker history', () => {
    it.each([true, false])(
        'shares floppy directories across volume and partition targets (desktop=%s)',
        async (desktop) => {
            const { floppyImportWorkflow: floppy, picker, requests } = setup(desktop);
            floppy.open(volume);
            const first = floppy.chooseWorkspace();
            expect(requests.at(-1)?.initialDirectory).toBeNull();
            expect(requests.at(-1)?.ondirectorychange).toBeTypeOf('function');
            requests.at(-1)?.ondirectorychange?.(disks);
            picker.finish(null);
            await first;
            await floppy.close();
            floppy.open(partition);
            const second = floppy.chooseWorkspace();
            expect(requests.at(-1)?.initialDirectory).toEqual(disks);
            expect(floppy.request).toMatchObject({ target: partition, mode: 'create', partitionIndex: 0 });
            picker.finish(null);
            await second;
            await floppy.close();
        },
    );

    it('restores the source folder when adding companions and keeps it after cancellation', async () => {
        const { floppyImportWorkflow: floppy, picker, requests } = setup();
        const choosing = floppy.chooseFiles(volume);
        requests.at(-1)?.ondirectorychange?.(disks);
        picker.finish(serverDirectoryLocation({ ...disks, relativePath: disks.relativePath + '/disk1' }));
        await choosing;
        const companion = floppy.chooseWorkspace();
        expect(requests.at(-1)?.initialDirectory).toEqual(disks);
        picker.finish(null);
        await companion;
        expect(floppy.request?.members).toHaveLength(1);
        await floppy.close();
        const reopened = floppy.chooseFiles(partition);
        expect(requests.at(-1)?.initialDirectory).toEqual(disks);
        picker.finish(null);
        await reopened;
    });

    it('clears an unavailable remembered directory and starts fresh for a new application instance', async () => {
        const { floppyImportWorkflow: floppy, picker, requests } = setup();
        let choosing = floppy.chooseFiles(volume);
        requests.at(-1)?.ondirectorychange?.(disks);
        picker.finish(null);
        await choosing;
        choosing = floppy.chooseFiles(partition);
        requests.at(-1)?.ondirectorychange?.(null);
        picker.finish(null);
        await choosing;
        choosing = floppy.chooseFiles(volume);
        expect(requests.at(-1)?.initialDirectory).toBeNull();
        picker.finish(null);
        await choosing;
        const fresh = setup();
        const opening = fresh.floppyImportWorkflow.chooseFiles(partition);
        expect(fresh.requests.at(-1)?.initialDirectory).toBeNull();
        fresh.picker.finish(null);
        await opening;
    });

    it('ignores directory callbacks from dismissed pickers even when the import stays open', async () => {
        const { floppyImportWorkflow: floppy, picker, requests } = setup(false);
        floppy.open(volume);
        let choosing = floppy.chooseWorkspace();
        const dismissed = requests.at(-1)!;
        dismissed.ondirectorychange?.(disks);
        picker.finish(null);
        await choosing;
        dismissed.ondirectorychange?.(packages);
        choosing = floppy.chooseWorkspace();
        expect(requests.at(-1)?.initialDirectory).toEqual(disks);
        picker.finish(null);
        await choosing;
        await floppy.close();
        floppy.open(partition);
        dismissed.ondirectorychange?.(null);
        choosing = floppy.chooseWorkspace();
        expect(requests.at(-1)?.initialDirectory).toEqual(disks);
        picker.finish(null);
        await choosing;
        await floppy.close();
    });

    it('keeps floppy history separate while single and batch packages share their directory', async () => {
        const {
            floppyImportWorkflow: floppy,
            packageImportWorkflow: single,
            packageBatchImportWorkflow: batch,
            picker,
            requests,
        } = setup();
        let choosing = floppy.chooseFiles(volume);
        requests.at(-1)?.ondirectorychange?.(disks);
        picker.finish(null);
        await choosing;
        single.open(volume);
        choosing = single.chooseWorkspace();
        expect(requests.at(-1)?.initialDirectory).toBeNull();
        requests.at(-1)?.ondirectorychange?.(packages);
        picker.finish(null);
        await choosing;
        await single.close();
        batch.open(partition);
        choosing = batch.chooseWorkspace();
        expect(requests.at(-1)?.initialDirectory).toEqual(packages);
        picker.finish(null);
        await choosing;
        await batch.close();
        choosing = floppy.chooseFiles(partition);
        expect(requests.at(-1)?.initialDirectory).toEqual(disks);
        picker.finish(null);
        await choosing;
    });
});
