import { describe, expect, it, vi } from 'vitest';
import { get } from 'svelte/store';

import type { ExportWorkflow } from '../export/workflow.svelte';
import type { PackageBatchImportWorkflow } from '../import/packageBatchWorkflow.svelte';
import type { DiskTreeItem } from '../../lib/types';
import { directComputerDialogVisible, DirectComputerWorkflow } from './directComputerWorkflow';

const target = { kind: 'volume', id: 'volume-1', name: 'Volume 1' } as DiskTreeItem;
const sample = {
    kind: 'SBNK',
    objectId: 'sample-1',
    name: 'Sample 1',
    typeLabel: 'Sample',
    partitionIndex: 0,
    partitionName: 'Partition 1',
    volumeName: 'Volume 1',
} as const;
const secondSample = { ...sample, objectId: 'sample-2', name: 'Sample 2' } as const;

function packageBatchImportWorkflow(): PackageBatchImportWorkflow {
    return {
        open: vi.fn(),
        chooseLocal: vi.fn(),
    } as unknown as PackageBatchImportWorkflow;
}

function wavExportWorkflow(): ExportWorkflow {
    return {
        audioRequest: null,
        requestWav: vi.fn(),
        requestWavToComputer: vi.fn().mockImplementation(async function (this: { audioRequest: unknown }) {
            this.audioRequest = { inspection: { issues: [] }, destinationFlow: 'DIRECT_COMPUTER' };
        }),
        audioToComputer: vi.fn(),
    } as unknown as ExportWorkflow;
}

describe('DirectComputerWorkflow', () => {
    it('opens the native package picker immediately for the managed local sidecar', () => {
        const workflow = packageBatchImportWorkflow();
        const coordinator = new DirectComputerWorkflow(true, 'local');

        coordinator.importPackages(workflow, target);

        expect(workflow.open).toHaveBeenCalledWith(target);
        expect(workflow.chooseLocal).toHaveBeenCalledWith(true);
        expect(get(coordinator.pendingOperation)).toBe('package-batch-import');
        expect(directComputerDialogVisible(get(coordinator.pendingOperation), 'package-batch-import', false)).toBe(
            false,
        );
    });

    it('retains the destination chooser for a configured remote connection', () => {
        const workflow = packageBatchImportWorkflow();

        new DirectComputerWorkflow(true, 'remote').importPackages(workflow, target);

        expect(workflow.open).toHaveBeenCalledWith(target);
        expect(workflow.chooseLocal).not.toHaveBeenCalled();
    });

    it('reveals package loading and validation after a native source is selected', () => {
        const workflow = packageBatchImportWorkflow();
        const coordinator = new DirectComputerWorkflow(true, 'local');

        coordinator.importPackages(workflow, target);

        expect(directComputerDialogVisible(get(coordinator.pendingOperation), 'package-batch-import', true)).toBe(true);
    });

    it('never suppresses dialogs for configured remote connections', () => {
        expect(directComputerDialogVisible(null, 'package-batch-import', false)).toBe(true);
    });

    it('opens one native directory picker after inspecting a direct multi-Sample WAV export', async () => {
        const workflow = wavExportWorkflow();

        await new DirectComputerWorkflow(true, 'local').exportWav(workflow, [sample, secondSample]);

        expect(workflow.requestWavToComputer).toHaveBeenCalledOnce();
        expect(workflow.requestWavToComputer).toHaveBeenCalledWith([sample, secondSample]);
        expect(workflow.audioToComputer).toHaveBeenCalledWith(true);
    });

    it('does not open a native directory picker when direct WAV inspection reports a fatal issue', async () => {
        const workflow = wavExportWorkflow();
        vi.mocked(workflow.requestWavToComputer).mockImplementationOnce(async function (this: {
            audioRequest: unknown;
        }) {
            this.audioRequest = {
                inspection: { issues: [{ fatal: true }] },
                destinationFlow: 'DIRECT_COMPUTER',
            };
        });

        await new DirectComputerWorkflow(true, 'local').exportWav(workflow, [sample]);

        expect(workflow.audioToComputer).not.toHaveBeenCalled();
    });

    it('retains the WAV destination chooser for a configured remote connection', async () => {
        const workflow = wavExportWorkflow();

        await new DirectComputerWorkflow(true, 'remote').exportWav(workflow, [sample]);

        expect(workflow.requestWav).toHaveBeenCalledWith([sample]);
        expect(workflow.requestWavToComputer).not.toHaveBeenCalled();
        expect(workflow.audioToComputer).not.toHaveBeenCalled();
    });
});
