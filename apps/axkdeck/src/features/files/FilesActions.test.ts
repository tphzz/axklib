import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { cleanup, fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { FilesController } from './controller.svelte';
import FilesView from './FilesView.svelte';
import FilesNavigation from './FilesNavigation.svelte';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import type { FilesystemMutationDriver } from '../../lib/filesystem';
import type { FilesystemImportActions, FilesystemImageImporter } from '../../lib/filesystemImport';
import { PhysicalPosition } from '@tauri-apps/api/dpi';
import { dispatchNativeFilesystemDrop } from '../../lib/nativeFilesystemDropTarget';

const native = vi.hoisted(() => ({ lstat: vi.fn(), invoke: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: native.invoke }));
vi.mock('@tauri-apps/plugin-fs', () => ({ lstat: native.lstat, open: vi.fn(), SeekMode: { Start: 0 } }));

afterEach(cleanup);

async function setup(
    writable = true,
    exportEnabled = false,
    importEnabled = false,
    imageImport?: FilesystemImageImporter,
) {
    const root = filesystemEntry({ id: 'root', name: 'Disk', path: '', kind: 'root', parentId: null, ancestorIds: [] });
    const folder = filesystemEntry();
    const file = filesystemEntry({
        id: 'file',
        kind: 'file',
        name: 'File',
        path: '/Documents/File',
        parentId: 'folder',
        ancestorIds: ['root', 'folder'],
    });
    const metadata = filesystemEntry({ id: 'metadata', name: 'Metadata', filesystemMetadata: true });
    const metadataFiles = ['sfserrlog', 'sfserram'].map((name) =>
        filesystemEntry({ id: name, name, path: `/${name}`, kind: 'file', filesystemMetadata: true }),
    );
    const controller = new FilesController({
        inspect: async (query = {}) => {
            const items = query.entryId
                ? [root, folder, file, metadata, ...metadataFiles].filter((entry) => entry.id === query.entryId)
                : query.parentId === 'root'
                  ? [folder, metadata, ...metadataFiles]
                  : query.parentId === 'folder'
                    ? [file]
                    : [root];
            return {
                revision: 2,
                available: true,
                filesystemName: 'Test filesystem',
                deviceView: null,
                rootCapabilities: [
                    {
                        ...writableFilesRoot,
                        createDirectory: writable,
                        putFile: writable,
                        deleteEntry: writable,
                        renameEntry: writable,
                        moveEntry: writable,
                    },
                ],
                items,
                totalCount: items.length,
            };
        },
    });
    await controller.initialize();
    const driver = {
        execute: vi
            .fn<FilesystemMutationDriver['execute']>()
            .mockResolvedValue({ jobId: 1, kind: 'images.filesystem.edit', status: 'completed' }),
        observe: vi.fn<FilesystemMutationDriver['observe']>(),
        cancel: vi.fn<FilesystemMutationDriver['cancel']>(),
        refresh: vi.fn<FilesystemMutationDriver['refresh']>().mockResolvedValue(),
    };
    const onexport = exportEnabled ? vi.fn() : undefined;
    const imports: FilesystemImportActions | undefined = importEnabled
        ? {
              supportsClientUploads: true,
              chooseFiles: vi.fn().mockResolvedValue(null),
              chooseDirectory: vi.fn().mockResolvedValue(null),
              upload: vi.fn(),
              release: vi.fn().mockResolvedValue(undefined),
              inspectInputs: vi.fn(),
              inspectDestination: vi.fn(),
              observe: vi.fn(),
              cancel: vi.fn().mockResolvedValue(undefined),
          }
        : undefined;
    return {
        controller,
        driver,
        onexport,
        imports,
        view: render(FilesView, { controller, driver, imports, onexport, imageImport }),
    };
}

describe('Files mutation controls', () => {
    it('does not turn semantic image import into permission to write raw files', async () => {
        const open = vi.fn().mockResolvedValue(false);
        const { view, imports, controller } = await setup(false, false, true, {
            label: 'Import floppy...',
            enabled: true,
            busy: false,
            open,
        });
        await fireEvent.drop(view.getByRole('treegrid'), {
            dataTransfer: { types: ['Files'], items: [], files: [new File(['x'], 'ordinary.dat')] },
        });
        await waitFor(() => expect(open).toHaveBeenCalledOnce());
        await waitFor(() => expect(controller.error).toContain('Raw file import is not available'));
        expect(imports!.upload).not.toHaveBeenCalled();
        expect(view.queryByRole('dialog')).toBeNull();
    });
    it('retains Refresh recovery when rename saved but listing failed', async () => {
        const { view, driver, controller } = await setup();
        await fireEvent.click(view.getAllByRole('row')[0]);
        await fireEvent.click(view.getByRole('button', { name: 'Rename...' }));
        await fireEvent.input(view.getByRole('textbox', { name: 'Name' }), { target: { value: 'Renamed' } });
        vi.spyOn(controller, 'initialize').mockImplementationOnce(async () => {
            controller.error = 'Listing failed';
        });
        await fireEvent.click(view.getByRole('button', { name: 'Rename' }));
        await waitFor(() =>
            expect(within(view.getByRole('dialog')).getByRole('alert').textContent).toContain(
                'Changes saved; refresh failed',
            ),
        );
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        expect(view.getByRole('dialog')).toBeTruthy();
        await fireEvent.click(within(view.getByRole('dialog')).getByRole('button', { name: 'Refresh' }));
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
        expect(driver.execute).toHaveBeenCalledOnce();
    });

    it('renames the selected directory through F2 and closes only after refresh', async () => {
        const { view, driver } = await setup();
        const row = view.getAllByRole('row')[0];
        await fireEvent.click(row);
        await fireEvent.keyDown(row, { key: 'F2' });
        const input = (await view.findByRole('textbox', { name: 'Name' })) as HTMLInputElement;
        expect(input.value).toBe('Documents');
        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(input.selectionStart).toBe(0);
        expect(input.selectionEnd).toBe(input.value.length);
        const rename = view.getByRole('button', { name: 'Rename' }) as HTMLButtonElement;
        expect(rename.disabled).toBe(true);
        await fireEvent.input(input, { target: { value: 'Renamed' } });
        let finish!: () => void;
        driver.refresh.mockImplementationOnce(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        await fireEvent.submit(input.closest('form')!);
        await waitFor(() => expect(driver.refresh).toHaveBeenCalledOnce());
        expect(view.queryByRole('dialog')).not.toBeNull();
        expect(driver.execute).toHaveBeenCalledWith(
            2,
            [{ kind: 'RENAME', entryId: 'folder', newName: 'Renamed' }],
            expect.any(Function),
        );
        finish();
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
    });

    it('lets a specialized importer consume a drop on a root metadata file without raw insertion', async () => {
        const open = vi.fn().mockResolvedValue(true);
        const { view, imports, driver } = await setup(true, false, true, {
            label: 'Import image...',
            enabled: true,
            busy: false,
            open,
        });
        await fireEvent.drop(view.getByText('sfserram').closest('[role="row"]')!, {
            dataTransfer: { types: ['Files'], items: [], files: [new File(['image'], 'disk.img')] },
        });
        await waitFor(() => expect(open).toHaveBeenCalledOnce());
        expect(open.mock.calls[0][0][0]).toMatchObject({ source: { name: 'disk.img' } });
        expect(imports!.upload).not.toHaveBeenCalled();
        expect(driver.execute).not.toHaveBeenCalled();
        expect(view.queryByRole('dialog', { name: 'Add files' })).toBeNull();
    });
    it('does not open a raw import after the destination revision changes during classification', async () => {
        let finish!: (consumed: boolean) => void;
        const open = vi.fn(
            () =>
                new Promise<boolean>((resolve) => {
                    finish = resolve;
                }),
        );
        const { controller, view, imports } = await setup(true, false, true, {
            label: 'Import image...',
            enabled: true,
            busy: false,
            open,
        });
        await fireEvent.drop(view.getByRole('treegrid'), {
            dataTransfer: { types: ['Files'], items: [], files: [new File(['x'], 'plain.raw')] },
        });
        await waitFor(() => expect(open).toHaveBeenCalledOnce());
        controller.revision = 3;
        finish(false);
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(imports!.upload).not.toHaveBeenCalled();
        expect(view.queryByRole('dialog')).toBeNull();
    });
    it('admits specialized browser drops outside the tree but not outside the workspace', async () => {
        const open = vi.fn().mockResolvedValue(true);
        const { view } = await setup(true, false, true, {
            label: 'Import image...',
            enabled: true,
            busy: false,
            open,
        });
        view.container.dataset.workspaceMode = 'files';
        const background = document.createElement('aside');
        view.container.append(background);
        const dataTransfer = { types: ['Files'], items: [], files: [new File(['image'], 'disk.img')] };
        await fireEvent.drop(document.body, { dataTransfer });
        expect(open).not.toHaveBeenCalled();
        await fireEvent.drop(background, { dataTransfer });
        await waitFor(() => expect(open).toHaveBeenCalledOnce());
    });
    it.each(['Clear selection', 'Escape', 'toggle'])(
        'creates at root after %s removes the final directory selection',
        async (method) => {
            const { view, driver, controller } = await setup();
            const row = view.getByText('Documents').closest('[role="row"]')!;
            await fireEvent.click(row);
            if (method === 'Clear selection') await fireEvent.click(view.getByRole('button', { name: method }));
            else if (method === 'Escape') await fireEvent.keyDown(row, { key: 'Escape' });
            else await fireEvent.click(row, { ctrlKey: true });
            expect(controller.selection).toEqual([]);
            await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
            const dialog = within(await view.findByRole('dialog', { name: 'New directory' }));
            expect(dialog.getByText('Disk')).toBeTruthy();
            await fireEvent.input(dialog.getByRole('textbox', { name: 'Directory name' }), {
                target: { value: 'New' },
            });
            await fireEvent.click(dialog.getByRole('button', { name: 'Create' }));
            await waitFor(() =>
                expect(driver.execute).toHaveBeenCalledWith(
                    2,
                    [{ kind: 'CREATE_DIRECTORY', parentEntryId: 'root', relativePath: ['New'] }],
                    expect.any(Function),
                ),
            );
        },
    );

    it.each(['sfserrlog', 'sfserram'])(
        'creates beside protected root file %s without permitting its deletion',
        async (name) => {
            const { view, driver } = await setup(true, false, true);
            await fireEvent.click(view.getByText(name).closest('[role="row"]')!);
            expect(
                (view.getByRole('button', { name: 'Delete selected entries...' }) as HTMLButtonElement).disabled,
            ).toBe(true);
            expect((view.getByRole('button', { name: 'Add files...' }) as HTMLButtonElement).disabled).toBe(false);
            const create = view.getByRole('button', { name: 'New directory...' }) as HTMLButtonElement;
            expect(create.disabled).toBe(false);
            await fireEvent.click(create);
            const dialog = within(await view.findByRole('dialog', { name: 'New directory' }));
            expect(dialog.getByText('Disk')).toBeTruthy();
            await fireEvent.input(dialog.getByRole('textbox', { name: 'Directory name' }), {
                target: { value: 'New' },
            });
            await fireEvent.click(dialog.getByRole('button', { name: 'Create' }));
            await waitFor(() =>
                expect(driver.execute).toHaveBeenCalledWith(
                    2,
                    [{ kind: 'CREATE_DIRECTORY', parentEntryId: 'root', relativePath: ['New'] }],
                    expect.any(Function),
                ),
            );
        },
    );

    it('does not use a keyboard-focused directory as a destination without a selection', async () => {
        const { view, controller } = await setup();
        const row = view.getByText('Documents').closest('[role="row"]')!;
        await fireEvent.keyDown(row, { key: 'Home', ctrlKey: true });
        expect(controller.selection).toEqual([]);
        await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
        expect(within(await view.findByRole('dialog')).getByText('Disk')).toBeTruthy();
    });

    it('keeps the actual selection as destination when keyboard focus moves to another row', async () => {
        const { view, controller } = await setup();
        const row = view.getByText('Documents').closest('[role="row"]')!;
        await fireEvent.click(row);
        await fireEvent.keyDown(row, { key: 'End', ctrlKey: true });
        expect(controller.selection.map((entry) => entry.id)).toEqual(['folder']);
        await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
        expect(within(await view.findByRole('dialog')).getByText('/Documents')).toBeTruthy();
    });

    it('captures the directory destination before dialog focus and selection changes', async () => {
        const { view, driver, controller } = await setup();
        await fireEvent.click(view.getByText('Documents').closest('[role="row"]')!);
        await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
        const dialog = within(await view.findByRole('dialog'));
        controller.clearSelection();
        await fireEvent.input(dialog.getByRole('textbox', { name: 'Directory name' }), { target: { value: 'New' } });
        await fireEvent.click(dialog.getByRole('button', { name: 'Create' }));
        await waitFor(() =>
            expect(driver.execute).toHaveBeenCalledWith(
                2,
                [{ kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['New'] }],
                expect.any(Function),
            ),
        );
    });

    it('routes native raw drops through the pointed directory and unregisters on unmount', async () => {
        const { controller, view, imports, driver } = await setup(true, false, true);
        const row = view.getByText('Documents').closest('[role="row"]')!;
        const point = vi.fn(() => row);
        Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: point });
        native.lstat.mockResolvedValue({ isFile: true, isDirectory: false, isSymlink: false, size: 0 });
        vi.mocked(imports!.upload).mockRejectedValue(new Error('Stop after review'));
        controller.select(filesystemEntry({ id: 'metadata', name: 'Metadata', filesystemMetadata: true }));
        const position = new PhysicalPosition(300, 150);
        dispatchNativeFilesystemDrop({ type: 'enter', paths: ['/EMPTY'], position }, 1.5);
        await waitFor(() => expect(row.classList.contains('drop-target')).toBe(true));
        expect(point).toHaveBeenCalledWith(200, 100);
        dispatchNativeFilesystemDrop({ type: 'drop', paths: ['/EMPTY'], position }, 1.5);
        const dialog = await view.findByRole('dialog', { name: 'Add files' });
        expect(within(dialog).getByText('/Documents')).toBeTruthy();
        await waitFor(() => expect(imports!.upload).toHaveBeenCalledOnce());
        expect(vi.mocked(imports!.upload).mock.calls[0][0][0]).toMatchObject({ name: 'EMPTY', size: 0 });
        expect(driver.execute).not.toHaveBeenCalled();
        expect(controller.selected?.id).toBe('metadata');
        view.unmount();
        point.mockClear();
        dispatchNativeFilesystemDrop({ type: 'drop', paths: ['/EMPTY'], position }, 1.5);
        expect(point).not.toHaveBeenCalled();
        Reflect.deleteProperty(document, 'elementFromPoint');
    });
    it('opens drop review in the pointed directory without using the selection or writing immediately', async () => {
        const { controller, view, imports, driver } = await setup(true, false, true);
        const row = view.getByText('Documents').closest('[role="row"]')!;
        controller.select(filesystemEntry({ id: 'metadata', name: 'Metadata', filesystemMetadata: true }));
        const dataTransfer = {
            types: ['Files'],
            items: [],
            files: [new File(['x'], 'unknown.raw')],
            dropEffect: 'none',
        };
        vi.mocked(imports!.upload).mockRejectedValue(new Error('Stop after target review'));
        await fireEvent.dragOver(row, { dataTransfer });
        expect(dataTransfer.dropEffect).toBe('copy');
        expect(row.classList.contains('drop-target')).toBe(true);
        await fireEvent.drop(row, { dataTransfer });
        const dialog = await view.findByRole('dialog', { name: 'Add files' });
        expect(within(dialog).getByText('/Documents')).toBeTruthy();
        await waitFor(() => expect(imports!.upload).toHaveBeenCalledOnce());
        expect(driver.execute).not.toHaveBeenCalled();
        expect(controller.selected?.id).toBe('metadata');
        expect(row.classList.contains('drop-target')).toBe(false);
    });
    it('rejects drops on metadata or read-only roots and keeps directory drops as directories', async () => {
        const { view, imports } = await setup(true, false, true);
        const dataTransfer = { types: ['Files'], items: [], files: [new File([], 'zero')], dropEffect: 'copy' };
        const row = view.getByTitle('Metadata').closest('[role="row"]')!;
        await fireEvent.dragOver(row, { dataTransfer });
        expect(dataTransfer.dropEffect).toBe('none');
        await fireEvent.drop(row, { dataTransfer });
        expect(view.queryByRole('dialog')).toBeNull();
        expect(imports!.upload).not.toHaveBeenCalled();
        const entry = {
            name: 'EMPTY',
            isDirectory: true,
            isFile: false,
            createReader: () => ({ readEntries: (done: (entries: unknown[]) => void) => done([]) }),
        };
        await fireEvent.drop(view.getByRole('treegrid'), {
            dataTransfer: { types: ['Files'], items: [{ kind: 'file', webkitGetAsEntry: () => entry }], files: [] },
        });
        const dialog = await view.findByRole('dialog', { name: 'Import from disk' });
        expect(within(dialog).getByText('Disk')).toBeTruthy();
        await waitFor(() => expect(imports!.inspectDestination).toHaveBeenCalledOnce());
        expect(imports!.upload).not.toHaveBeenCalled();
        cleanup();
        const readOnly = await setup(false, false, true);
        await fireEvent.drop(readOnly.view.getByRole('treegrid'), { dataTransfer });
        expect(readOnly.view.queryByRole('dialog')).toBeNull();
    });
    it('offers partition Import before Export and opens the review in that exact root', async () => {
        const { controller, view, imports, onexport } = await setup(true, true, true);
        const navigation = render(FilesNavigation, {
            controller,
            onexport,
            onimport: (root) => view.component.importRoot(root),
        });
        await fireEvent.contextMenu(navigation.getByRole('button', { name: 'Disk' }));
        expect(navigation.getAllByRole('menuitem').map((item) => item.textContent?.trim())).toEqual([
            'Import',
            'Export',
        ]);
        const parent = navigation.getByRole('menuitem', { name: 'Import' });
        parent.focus();
        await fireEvent.keyDown(parent, { key: 'ArrowRight' });
        const child = navigation.getByRole('menuitem', { name: 'Import from disk...' });
        expect(document.activeElement).toBe(child);
        await fireEvent.click(child);
        await waitFor(() => expect(imports!.chooseDirectory).toHaveBeenCalledOnce());
        const dialog = await view.findByRole('dialog', { name: 'Import from disk' });
        expect(within(dialog).getByText('Disk')).toBeTruthy();
    });
    it('does not expose partition import for a read-only root', async () => {
        const { controller, onexport } = await setup(false, true, true);
        const onimport = vi.fn();
        const navigation = render(FilesNavigation, { controller, onexport, onimport });
        await fireEvent.contextMenu(navigation.getByRole('button', { name: 'Disk' }));
        expect(navigation.queryByRole('menuitem', { name: 'Import' })).toBeNull();
    });
    it('opens Add files in the selected file parent and preserves mutation/import/export ordering', async () => {
        const { view } = await setup(true, true, true);
        await fireEvent.click(view.getByRole('button', { name: 'Expand Documents' }));
        await fireEvent.contextMenu(view.getAllByRole('row')[1]);
        expect(view.getAllByRole('menuitem').map((item) => item.textContent?.trim())).toEqual([
            'New directory...',
            'Rename…',
            'Delete…',
            'Import',
            'Export',
        ]);
        const parent = view.getByRole('menuitem', { name: 'Import' });
        await fireEvent.focus(parent);
        parent.focus();
        await fireEvent.keyDown(parent, { key: 'ArrowRight' });
        const action = view.getByRole('menuitem', { name: 'Add files...' });
        expect(view.getByRole('menuitem', { name: 'Import from disk...' })).toBeTruthy();
        expect(document.activeElement).toBe(action);
        await fireEvent.keyDown(action, { key: 'ArrowLeft' });
        await waitFor(() => expect(document.activeElement).toBe(parent));
        await fireEvent.click(parent);
        await fireEvent.click(view.getByRole('menuitem', { name: 'Add files...' }));
        const dialog = await view.findByRole('dialog', { name: 'Add files' });
        expect(within(dialog).getByText('/Documents')).toBeTruthy();
    });

    it('does not offer Add files for a readonly destination', async () => {
        const { view } = await setup(false, false, true);
        expect((view.getByRole('button', { name: 'Add files...' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('exports the complete multi-selection and keeps the standard mutation/transfer ordering', async () => {
        const { view, onexport } = await setup(true, true);
        await fireEvent.click(view.getByRole('button', { name: 'Expand Documents' }));
        await fireEvent.click(view.getAllByRole('row')[0]);
        await fireEvent.click(view.getAllByRole('row')[1], { ctrlKey: true });
        await fireEvent.contextMenu(view.getAllByRole('row')[1]);
        expect(view.getAllByRole('menuitem').map((item) => item.textContent?.trim())).toEqual([
            'Delete 2 entries…',
            'Export',
        ]);
        expect(view.getAllByRole('separator')).toHaveLength(1);
        await fireEvent.click(view.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(view.getByRole('menuitem', { name: 'Export to disk...' }));
        expect(onexport).toHaveBeenCalledWith([
            expect.objectContaining({ id: 'folder' }),
            expect.objectContaining({ id: 'file' }),
        ]);
    });

    it('offers keyboard-accessible partition export and restores invoking focus', async () => {
        const { controller, onexport } = await setup(false, true);
        const navigation = render(FilesNavigation, { controller, onexport });
        const root = navigation.getByRole('button', { name: 'Disk' });
        root.focus();
        await fireEvent.keyDown(root, { key: 'F10', shiftKey: true });
        await fireEvent.keyDown(navigation.getByRole('menuitem', { name: 'Export' }), {
            key: 'ArrowRight',
        });
        const child = navigation.getByRole('menuitem', { name: 'Export to disk...' });
        expect(document.activeElement).toBe(child);
        await fireEvent.click(child);
        expect(onexport).toHaveBeenCalledWith([expect.objectContaining({ id: 'root' })]);
        await waitFor(() => expect(document.activeElement).toBe(root));
    });
    it('exports a read-only root or selection through the toolbar and shared Export submenu', async () => {
        const { view, onexport } = await setup(false, true);
        await fireEvent.click(view.getByRole('button', { name: 'Export to disk...' }));
        expect(onexport).toHaveBeenLastCalledWith([expect.objectContaining({ id: 'root' })]);
        const row = view.getAllByRole('row')[0];
        await fireEvent.contextMenu(row);
        const parent = view.getByRole('menuitem', { name: 'Export' });
        await fireEvent.keyDown(parent, { key: 'ArrowRight' });
        const child = view.getByRole('menuitem', { name: 'Export to disk...' });
        expect(document.activeElement).toBe(child);
        await fireEvent.click(child);
        expect(onexport).toHaveBeenLastCalledWith([expect.objectContaining({ id: 'folder' })]);
        await fireEvent.click(view.getAllByRole('row')[1]);
        expect((view.getByRole('button', { name: 'Export to disk...' }) as HTMLButtonElement).disabled).toBe(true);
    });
    it('shows the active command and waits for cancellation before offering refresh', async () => {
        const { view, driver } = await setup();
        let finish!: () => void;
        driver.execute.mockImplementationOnce(async (_revision, _edits, update) => {
            const job = { jobId: 8, kind: 'images.filesystem.edit', status: 'running' as const };
            update(job);
            return new Promise((resolve) => {
                finish = () => resolve({ ...job, status: 'cancelled' });
            });
        });
        await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
        await fireEvent.input(await view.findByRole('textbox', { name: 'Directory name' }), {
            target: { value: 'New' },
        });
        await fireEvent.click(view.getByRole('button', { name: 'Create' }));
        await waitFor(() =>
            expect((view.getByRole('button', { name: 'Create' }) as HTMLButtonElement).disabled).toBe(true),
        );
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        expect(driver.cancel).toHaveBeenCalledWith(8);
        expect(view.getByRole('dialog')).toBeTruthy();
        finish();
        await waitFor(() => expect(view.getByText('Cancelled')).toBeTruthy());
        await fireEvent.click(view.getByRole('button', { name: 'Refresh' }));
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
        expect(driver.execute).toHaveBeenCalledOnce();
    });
    it.each(['create', 'delete', 'rename'])(
        'keeps %s footer actions at the shared height and margins',
        async (kind) => {
            const styles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
            const geometry = styles.match(/\.secondary-button,\s*\.primary-button,\s*\.danger-button\s*\{[^}]+\}/)?.[0];
            const footer = styles.match(
                /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
            )?.[0];
            expect(geometry).toBeDefined();
            expect(footer).toBeDefined();
            expect(footer).toContain('font-size: var(--dialog-body-font-size)');
            const style = document.createElement('style');
            style.textContent = `${geometry}\n${footer}`;
            document.head.append(style);
            try {
                const { view } = await setup();
                await fireEvent.click(view.getAllByRole('row')[0]);
                await fireEvent.click(
                    view.getByRole('button', {
                        name:
                            kind === 'create'
                                ? 'New directory...'
                                : kind === 'rename'
                                  ? 'Rename...'
                                  : 'Delete selected entries...',
                    }),
                );
                const dialog = within(await view.findByRole('dialog'));
                for (const label of [
                    'Cancel',
                    kind === 'create' ? 'Create' : kind === 'rename' ? 'Rename' : 'Delete permanently',
                ]) {
                    const button = dialog.getByRole('button', { name: label });
                    expect(button.closest('.dialog-footer')).not.toBeNull();
                    const computed = getComputedStyle(button);
                    expect(computed.height).toBe('30px');
                    expect(computed.marginTop).toBe('0px');
                    expect(computed.marginBottom).toBe('0px');
                }
            } finally {
                style.remove();
            }
        },
    );
    it('opens a compact create dialog, validates the name and submits to the selected file parent', async () => {
        const { controller, driver, view } = await setup();
        await fireEvent.click(view.getByRole('button', { name: 'Expand Documents' }));
        await fireEvent.click(view.getByText('File', { selector: '.file-copy span' }));
        await fireEvent.click(view.getByRole('button', { name: 'New directory...' }));
        const input = await view.findByRole('textbox', { name: 'Directory name' });
        expect(input.classList.contains('dialog-field-control')).toBe(true);
        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(view.getByText(/Raw filesystem changes can break/)).toBeTruthy();
        const submit = view.getByRole('button', { name: 'Create' }) as HTMLButtonElement;
        expect(submit.disabled).toBe(true);
        await fireEvent.input(input, { target: { value: '../bad' } });
        expect(submit.disabled).toBe(true);
        await fireEvent.input(input, { target: { value: 'New' } });
        expect(submit.disabled).toBe(false);
        await fireEvent.click(submit);
        await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
        expect(driver.execute).toHaveBeenCalledWith(
            controller.revision,
            [{ kind: 'CREATE_DIRECTORY', parentEntryId: 'folder', relativePath: ['New'] }],
            expect.any(Function),
        );
        expect(driver.refresh).toHaveBeenCalledOnce();
    });

    it('uses the shared menu with mutation order and keyboard focus restoration', async () => {
        const { view, driver } = await setup();
        const row = view.getAllByRole('row')[0];
        row.focus();
        await fireEvent.keyDown(row, { key: 'F10', shiftKey: true });
        expect(view.getAllByRole('menuitem').map((item) => item.textContent?.trim())).toEqual([
            'New directory...',
            'Rename…',
            'Delete…',
        ]);
        await fireEvent.keyDown(view.getByRole('menu'), { key: 'End' });
        expect(document.activeElement).toBe(view.getByRole('menuitem', { name: 'Delete…' }));
        await fireEvent.keyDown(view.getByRole('menu'), { key: 'Escape' });
        await waitFor(() => expect(document.activeElement).toBe(row));
        await fireEvent.keyDown(row, { key: 'Delete' });
        expect(view.getByText(/all contents of selected directories/)).toBeTruthy();
        expect(driver.execute).not.toHaveBeenCalled();
        await fireEvent.click(view.getByRole('button', { name: 'Delete permanently' }));
        await waitFor(() =>
            expect(driver.execute).toHaveBeenCalledWith(
                2,
                [{ kind: 'DELETE', entryId: 'folder', recursive: true }],
                expect.any(Function),
            ),
        );
    });

    it('disables writes for read-only roots and metadata entries', async () => {
        const { view } = await setup(false);
        expect((view.getByRole('button', { name: 'New directory...' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.click(view.getAllByRole('row')[0]);
        expect((view.getByRole('button', { name: 'Delete selected entries...' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
        cleanup();
        const writable = await setup();
        await fireEvent.click(writable.view.getAllByRole('row')[1]);
        expect((writable.view.getByRole('button', { name: 'New directory...' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
        expect(
            (writable.view.getByRole('button', { name: 'Delete selected entries...' }) as HTMLButtonElement).disabled,
        ).toBe(true);
    });
});
