import { cleanup, fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { FilesExportWorkflow } from './exportWorkflow.svelte';
import FilesExportDialog from './FilesExportDialog.svelte';
import { filesystemEntry } from '../../lib/testing/filesystem';
import type { FilesystemExportActions } from '../../lib/filesystemExport';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

afterEach(cleanup);
async function setup(directComputer = true) {
    const driver: FilesystemExportActions = {
        directComputer,
        desktop: true,
        inspect: vi.fn().mockResolvedValue({
            rootDirectory: { entryId: 'root', sourcePath: '/Documents', name: 'Documents' },
            imageId: 'image',
            revision: 7,
            totalBytes: 4,
            entries: [
                {
                    entryId: 'file',
                    relativePath: ['file'],
                    sourcePath: '/Documents/file',
                    sizeBytes: 4,
                    directory: false,
                },
            ],
            notices: [{ entryId: 'meta', sourcePath: '/sfserrlog', message: 'Filesystem metadata omitted' }],
        }),
        chooseDestination: vi.fn().mockResolvedValue(null),
        execute: vi.fn(),
        observe: vi.fn(),
        cancel: vi.fn(),
        release: vi.fn().mockResolvedValue(undefined),
    };
    const workflow = new FilesExportWorkflow();
    await workflow.open(7, [filesystemEntry()], driver);
    return { workflow, driver, view: render(FilesExportDialog, { workflow }) };
}

describe('Files export review', () => {
    it('supports tree keyboard expansion without changing the exported selection', async () => {
        const { view, workflow, driver } = await setup();
        const tree = view.getByRole('treegrid', { name: 'Export entries' });
        const root = within(tree).getByRole('button', { name: 'Collapse Documents' }).closest('[role="row"]')!;
        await fireEvent.focus(root);
        await fireEvent.keyDown(root, { key: 'ArrowLeft' });
        await waitFor(() => expect(view.queryByTitle('Documents/file')).toBeNull());
        await fireEvent.keyDown(root, { key: 'ArrowRight' });
        await waitFor(() => expect(view.getByTitle('Documents/file')).toBeTruthy());
        await fireEvent.keyDown(root, { key: 'End' });
        await waitFor(() => expect(document.activeElement?.textContent).toContain('file'));
        await fireEvent.keyDown(document.activeElement!, { key: 'ArrowLeft' });
        await waitFor(() => expect(document.activeElement).toBe(root));
        expect(workflow.inspection!.entries).toHaveLength(1);
        expect(driver.execute).not.toHaveBeenCalled();
    });

    it('retains completed notices with Done rather than a disabled Export action', async () => {
        const { view, workflow } = await setup();
        workflow.phase = 'completed';
        workflow.message = 'Exported 2 entries; 1 notices';
        await waitFor(() => expect(view.getByRole('button', { name: 'Done' })).toBeTruthy());
        expect(view.queryByRole('button', { name: 'Export...' })).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Done' }));
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
    });

    it('keeps Cancel active during native saving and waits for cleanup before closing', async () => {
        const { view, workflow, driver } = await setup();
        let fail!: (error: Error) => void;
        const cancelPublication = vi.fn().mockResolvedValue(undefined);
        vi.mocked(driver.chooseDestination).mockResolvedValue({
            destination: { kind: 'DOWNLOAD', directoryName: 'Files' },
            publish: () =>
                new Promise((_resolve, reject) => {
                    fail = reject;
                }),
            cancelPublication,
        });
        vi.mocked(driver.execute).mockResolvedValue({
            jobId: 12,
            kind: 'images.filesystem.export',
            status: 'completed',
            result: { ...workflow.inspection!, destination: 'DOWNLOAD', output: null, download: null },
        });
        await fireEvent.click(view.getByRole('button', { name: 'Export...' }));
        await waitFor(() => expect(workflow.phase).toBe('saving'));
        expect(view.getByRole('button', { name: 'Cancel' }).hasAttribute('disabled')).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        expect(cancelPublication).toHaveBeenCalledOnce();
        expect(view.getByRole('button', { name: 'Cancel' }).hasAttribute('disabled')).toBe(true);
        expect(view.getByRole('dialog')).toBeTruthy();
        fail(new Error('Directory export cancelled'));
        await waitFor(() => expect(workflow.phase).toBe('failed'));
        await fireEvent.click(within(view.container.querySelector('footer')!).getByRole('button', { name: 'Close' }));
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
    });

    it('bounds entry and notice rows without truncating the reviewed export', async () => {
        const { view, workflow, driver } = await setup();
        const inspection = workflow.inspection!;
        workflow.inspection = {
            ...inspection,
            entries: Array.from({ length: 205 }, (_, index) => ({
                ...inspection.entries[0],
                entryId: `file-${index}`,
                relativePath: [`File ${index}`],
            })),
            notices: Array.from({ length: 201 }, (_, index) => ({
                entryId: `notice-${index}`,
                sourcePath: `/Notice ${index}`,
                message: 'Host name adjusted',
            })),
        };
        await waitFor(() => expect(view.container.querySelectorAll('.export-rows [role="row"]')).toHaveLength(100));
        expect(view.getByText('206 entries')).toBeTruthy();
        expect(view.container.querySelectorAll('.export-notices li')).toHaveLength(100);
        const entries = within(view.getByRole('navigation', { name: 'Export entry pages' }));
        const notices = within(view.getByRole('navigation', { name: 'Export notice pages' }));
        expect(entries.getByRole('button', { name: 'Previous page' }).hasAttribute('disabled')).toBe(true);
        const rows = view.container.querySelector('.export-rows')!;
        rows.scrollTop = 100;
        await fireEvent.click(entries.getByRole('button', { name: 'Next page' }));
        expect(rows.scrollTop).toBe(0);
        expect(view.getByText('File 100')).toBeTruthy();
        expect(view.queryByText('File 0')).toBeNull();
        expect(view.getByText('/Notice 0: Host name adjusted')).toBeTruthy();
        await fireEvent.click(entries.getByRole('button', { name: 'Next page' }));
        expect(view.container.querySelectorAll('.export-rows [role="row"]')).toHaveLength(7);
        expect(entries.getByRole('button', { name: 'Next page' }).hasAttribute('disabled')).toBe(true);
        const noticeRows = view.getByRole('list', { name: 'Export notices' });
        noticeRows.scrollTop = 100;
        await fireEvent.click(notices.getByRole('button', { name: 'Next page' }));
        expect(noticeRows.scrollTop).toBe(0);
        await fireEvent.click(notices.getByRole('button', { name: 'Next page' }));
        expect(view.container.querySelectorAll('.export-notices li')).toHaveLength(1);
        expect(view.getByText('/Notice 200: Host name adjusted')).toBeTruthy();
        expect(notices.getByRole('button', { name: 'Next page' }).hasAttribute('disabled')).toBe(true);
        expect(workflow.inspection.entries).toHaveLength(205);
        expect(workflow.inspection.notices).toHaveLength(201);
        await fireEvent.click(view.getByRole('button', { name: 'Export...' }));
        expect(driver.chooseDestination).toHaveBeenCalledWith('computer', 'Documents');
        workflow.inspection = inspection;
        await waitFor(() => expect(view.getByTitle('Documents/file')).toBeTruthy());
        expect(view.container.querySelector('.export-pagination.single-page')).toBeTruthy();
    });

    it('retains matching computed geometry for the mixed footer actions', async () => {
        const css = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
        const geometry = css.match(/\.secondary-button,\s*\.primary-button,\s*\.danger-button\s*\{[^}]+\}/)?.[0];
        const footer = css.match(
            /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
        )?.[0];
        expect(geometry).toBeDefined();
        expect(footer).toBeDefined();
        const style = document.createElement('style');
        style.textContent = `${geometry}\n${footer}`;
        document.head.append(style);
        try {
            const { view } = await setup();
            for (const label of ['Cancel', 'Export...']) {
                const computed = getComputedStyle(view.getByRole('button', { name: label }));
                expect(computed.height).toBe('30px');
                expect(computed.marginTop).toBe('0px');
                expect(computed.marginBottom).toBe('0px');
            }
        } finally {
            style.remove();
        }
    });
    it('keeps local exports in one direct route and displays the exact paths and omission notices', async () => {
        const { view, driver } = await setup();
        expect(view.getByRole('dialog', { name: 'Export files' })).toBeTruthy();
        expect(view.getByTitle('Documents/file')).toBeTruthy();
        expect(view.getByText(/Filesystem metadata omitted/)).toBeTruthy();
        expect(view.queryByText('Storage location')).toBeNull();
        expect(view.queryByText('This computer')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Export...' }));
        expect(driver.chooseDestination).toHaveBeenCalledWith('computer', 'Documents');
    });

    it('uses the shared destination chooser for a remote connection', async () => {
        const { view, driver } = await setup(false);
        await fireEvent.click(view.getByRole('button', { name: /Storage location/ }));
        expect(driver.chooseDestination).toHaveBeenCalledWith('workspace', 'Documents');
        await fireEvent.click(view.getByRole('button', { name: /This computer/ }));
        expect(driver.chooseDestination).toHaveBeenLastCalledWith('computer', 'Documents');
    });

    it('uses shared footer controls and closes with Escape', async () => {
        const { view, workflow } = await setup();
        await waitFor(() => expect(document.activeElement).toBe(view.getByRole('button', { name: 'Cancel' })));
        expect(view.getByRole('button', { name: 'Cancel' }).classList.contains('secondary-button')).toBe(true);
        expect(view.getByRole('button', { name: 'Export...' }).classList.contains('primary-button')).toBe(true);
        expect(view.getByRole('button', { name: 'Export...' }).parentElement?.classList.contains('dialog-footer')).toBe(
            true,
        );
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        await waitFor(() => expect(workflow.review).toBeNull());
    });
});
