import { describe, expect, it, vi } from 'vitest';
import { get } from 'svelte/store';

import type { PackageBatchImportWorkflow } from '../import/packageBatchWorkflow.svelte';
import type { DiskTreeItem } from '../../lib/types';
import { directComputerDialogVisible, DirectComputerWorkflow } from './directComputerWorkflow';

const target = { kind: 'volume', id: 'volume-1', name: 'Volume 1' } as DiskTreeItem;

function packageBatchImportWorkflow(): PackageBatchImportWorkflow {
    return {
        open: vi.fn(),
        chooseLocal: vi.fn(),
    } as unknown as PackageBatchImportWorkflow;
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
});
