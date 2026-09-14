import { describe, expect, it } from 'vitest';
import { fireEvent, render } from '@testing-library/svelte';
import { tick } from 'svelte';
import FloppyImportDialog from './FloppyImportDialog.svelte';
import { floppyDialogFixture } from '../../test/floppyDialogFixture';

describe('Floppy import dialog', () => {
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
