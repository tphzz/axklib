import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render } from '@testing-library/svelte';
import { tick } from 'svelte';
import FloppyImportDialog from './FloppyImportDialog.svelte';
import { floppyDialogFixture } from '../../test/floppyDialogFixture';
import { capacityConflict } from '../../test/importCapacityFixture';

describe('Floppy import dialog', () => {
    it('replaces incomplete totals with capacity summaries until a successful review', async () => {
        const workflow = floppyDialogFixture(5);
        const validPlan = workflow.request!.plan!;
        validPlan.allocation = [
            {
                partitionIndex: 0,
                groupName: '',
                volumeName: 'Target',
                rawGroup: '',
                rawVolume: '',
                insertedObjectCount: 5,
                reusedObjectCount: 0,
                blockedObjectCount: 0,
                payloadClusters: 5,
                payloadSectors: 0,
                continuationClusters: 0,
                directoryGrowthBytes: 0,
                directoryGrowthClusters: 0,
                directoryContinuationClusters: 0,
                infrastructureClusters: 0,
                additionalAllocatedBytes: 5120,
                remainingObjectIds: 10,
                remainingClusters: 100,
                projectedImageSectors: 0,
                projectedImageSizeBytes: 0,
            },
        ];
        workflow.request!.plan = {
            ...validPlan,
            valid: false,
            conflicts: Array.from({ length: 63 }, (_, i) => capacityConflict(0, `wave-${i}`)),
        };
        const view = render(FloppyImportDialog, { workflow });
        expect(view.getAllByText('Not enough space on Partition 1')).toHaveLength(1);
        expect(view.queryByText('Insert')).toBeNull();
        expect(view.queryByText('Reuse')).toBeNull();
        expect(view.getByRole('button', { name: 'Import' }).hasAttribute('disabled')).toBe(true);
        workflow.request!.plan = validPlan;
        await tick();
        expect(view.queryByText(/Not enough space/)).toBeNull();
        expect(view.getByText('Insert')).toBeTruthy();
        expect(view.getByText('Reuse')).toBeTruthy();
        expect(view.getByRole('button', { name: 'Import' }).hasAttribute('disabled')).toBe(false);
    });
    it('keeps direct source recovery free of the local/remote chooser', async () => {
        const workflow = floppyDialogFixture(5, true);
        workflow.request!.members = [];
        workflow.request!.inspection = null;
        workflow.request!.error = 'Picker failed';
        const choose = vi.spyOn(workflow, 'chooseWorkspace').mockResolvedValue();
        const view = render(FloppyImportDialog, { workflow });
        expect(view.queryByText('Storage location')).toBeNull();
        expect(view.queryByText('This computer')).toBeNull();
        expect(view.getByRole('alert').textContent).toBe('Picker failed');
        await fireEvent.click(view.getByRole('button', { name: 'Choose floppy images' }));
        expect(choose).toHaveBeenCalledWith(true);
    });
    it('offers one direct companion action after source selection', async () => {
        const workflow = floppyDialogFixture(5, true);
        const choose = vi.spyOn(workflow, 'chooseWorkspace').mockResolvedValue();
        const view = render(FloppyImportDialog, { workflow });
        expect(view.queryByRole('button', { name: 'Workspace' })).toBeNull();
        expect(view.queryByRole('button', { name: 'Computer' })).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Add floppy images' }));
        expect(choose).toHaveBeenCalledWith();
    });
    it('keeps all footer actions visible but disabled during preparation and writing', async () => {
        const workflow = floppyDialogFixture(5);
        workflow.request!.status = 'applying';
        const view = render(FloppyImportDialog, { workflow });
        for (const name of ['Cancel', 'Review', 'Import'])
            expect(view.getByRole('button', { name }).hasAttribute('disabled')).toBe(true);
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        await workflow.close();
        expect(workflow.request).not.toBeNull();
        workflow.completion.phase = 'importing';
        await tick();
        expect(view.container.querySelectorAll('.dialog-footer-actions button')).toHaveLength(3);
    });
    it('keeps the established footer order, shared controls and separately bounded rows', async () => {
        const workflow = floppyDialogFixture(5),
            view = render(FloppyImportDialog, { workflow });
        expect(
            [...view.container.querySelectorAll('.dialog-footer-actions button')].map((b) => b.textContent?.trim()),
        ).toEqual(['Cancel', 'Review', 'Import']);
        expect(view.getByRole('button', { name: 'Existing' })).toBeTruthy();
        expect(view.getByRole('button', { name: /^New$/ })).toBeTruthy();
        expect(view.getByRole('checkbox', { name: 'Select all objects' }).classList.contains('dialog-checkbox')).toBe(
            true,
        );
        expect(view.container.querySelector('.floppy-rows .floppy-table-heading')).toBeNull();
        expect(view.container.querySelector('.floppy-results')).toBeNull();
        await fireEvent.click(view.getByRole('checkbox', { name: 'Select all objects' }));
        expect(workflow.request?.selected).toEqual([]);
        expect(view.getByRole('button', { name: /^Import$/ }).hasAttribute('disabled')).toBe(true);
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        await tick();
        expect(workflow.request).toBeNull();
    });
    it('uses the package source chooser before showing destination and review controls', () => {
        const workflow = floppyDialogFixture();
        workflow.request!.members = [];
        workflow.request!.inspection = null;
        const view = render(FloppyImportDialog, { workflow });
        expect(view.container.querySelector('.import-source-choice')).toBeTruthy();
        expect(view.getByRole('button', { name: /Storage location/ })).toBeTruthy();
        expect(view.getByRole('button', { name: /This computer/ })).toBeTruthy();
        expect(view.queryByRole('button', { name: 'Review' })).toBeNull();
        expect(view.queryByRole('button', { name: 'Existing' })).toBeNull();
    });
});
