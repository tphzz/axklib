import { describe, expect, it, vi } from 'vitest';
import { get } from 'svelte/store';

import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
import type { DiskTreeItem } from '../../lib/types';
import { directComputerDialogVisible, DirectComputerWorkflow } from './directComputerWorkflow';

const target = { kind: 'volume', id: 'volume-1', name: 'Volume 1' } as DiskTreeItem;

function packageImportWorkflow(): PackageImportWorkflow {
    return {
        open: vi.fn(),
        chooseLocal: vi.fn(),
    } as unknown as PackageImportWorkflow;
}

describe('DirectComputerWorkflow', () => {
    it('opens the native package picker immediately for the managed local sidecar', () => {
        const workflow = packageImportWorkflow();
        const coordinator = new DirectComputerWorkflow(true, 'local');

        coordinator.importPackage(workflow, target);

        expect(workflow.open).toHaveBeenCalledWith(target);
        expect(workflow.chooseLocal).toHaveBeenCalledWith(true);
        expect(get(coordinator.pendingOperation)).toBe('package-import');
        expect(directComputerDialogVisible(get(coordinator.pendingOperation), 'package-import', false)).toBe(false);
    });

    it('retains the destination chooser for a configured remote connection', () => {
        const workflow = packageImportWorkflow();

        new DirectComputerWorkflow(true, 'remote').importPackage(workflow, target);

        expect(workflow.open).toHaveBeenCalledWith(target);
        expect(workflow.chooseLocal).not.toHaveBeenCalled();
    });

    it('reveals package loading and validation after a native source is selected', () => {
        const workflow = packageImportWorkflow();
        const coordinator = new DirectComputerWorkflow(true, 'local');

        coordinator.importPackage(workflow, target);

        expect(directComputerDialogVisible(get(coordinator.pendingOperation), 'package-import', true)).toBe(true);
    });

    it('never suppresses dialogs for configured remote connections', () => {
        expect(directComputerDialogVisible(null, 'package-import', false)).toBe(true);
    });
});
