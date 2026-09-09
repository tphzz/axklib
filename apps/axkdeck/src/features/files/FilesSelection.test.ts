import { cleanup, fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import FilesView from './FilesView.svelte';
import { FilesController } from './controller.svelte';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';
import type { FilesystemMutationDriver } from '../../lib/filesystem';

afterEach(cleanup);

async function setup() {
    const root = filesystemEntry({ id: 'root', name: 'Disk', path: '', kind: 'root', parentId: null, ancestorIds: [] });
    const files = ['A', 'B', 'C', 'D'].map((name) =>
        filesystemEntry({ id: name, name, path: `/${name}`, kind: 'file' }),
    );
    const controller = new FilesController({
        inspect: async (query = {}) => {
            const items = query.parentId ? files : [root];
            return {
                revision: 7,
                available: true,
                deviceView: null,
                filesystemName: 'Test',
                items,
                totalCount: items.length,
                rootCapabilities: [writableFilesRoot],
            };
        },
    });
    await controller.initialize();
    const driver: FilesystemMutationDriver = {
        execute: vi.fn().mockResolvedValue({ jobId: 1, kind: 'images.filesystem.edit', status: 'completed' }),
        observe: vi.fn(),
        cancel: vi.fn(),
        refresh: vi.fn().mockResolvedValue(undefined),
    };
    const view = render(FilesView, { controller, driver });
    return { controller, driver, view, rows: view.getAllByRole('row') };
}

describe('Files multiselection interactions', () => {
    it('clears selection on empty tree background without resetting the tree context', async () => {
        const { rows, controller, view } = await setup();
        await fireEvent.click(rows[0]);
        const tree = view.getByRole('treegrid');
        tree.scrollTop = 40;
        await fireEvent.scroll(tree);
        await fireEvent.pointerDown(tree, { button: 0 });
        await fireEvent.click(tree);
        expect(controller.selection).toEqual([]);
        expect(controller.scrollTop).toBe(40);
        expect(controller.rows.map((row) => row.entry.id)).toEqual(['A', 'B', 'C', 'D']);
    });

    it('preserves selection when interacting with search, toolbar actions and dialog content', async () => {
        const { rows, controller, view } = await setup();
        await fireEvent.click(rows[0]);
        const search = view.getByRole('searchbox', { name: 'Search filesystem' });
        await fireEvent.pointerDown(search, { button: 0 });
        await fireEvent.click(search);
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A']);
        const create = view.getByRole('button', { name: 'New directory...' });
        await fireEvent.pointerDown(create, { button: 0 });
        await fireEvent.click(create);
        const dialog = await view.findByRole('dialog');
        await fireEvent.pointerDown(dialog, { button: 0 });
        await fireEvent.click(dialog);
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A']);
    });

    it('preserves a pointer batch in the context menu and submits one confirmed deletion job', async () => {
        const { view, rows, driver } = await setup();
        expect(view.getByRole('treegrid').getAttribute('aria-multiselectable')).toBe('true');
        await fireEvent.click(rows[0]);
        await fireEvent.click(rows[2], { ctrlKey: true });
        expect(rows.map((row) => row.getAttribute('aria-selected'))).toEqual(['true', 'false', 'true', 'false']);
        expect(view.getByText('2 selected')).toBeTruthy();
        await fireEvent.contextMenu(rows[0]);
        await fireEvent.click(view.getByRole('menuitem', { name: 'Delete 2 entries…' }));
        const dialog = view.getByRole('dialog');
        expect(dialog.textContent).toContain('/A');
        expect(dialog.textContent).toContain('/C');
        expect(driver.execute).not.toHaveBeenCalled();
        await fireEvent.click(view.getByRole('button', { name: 'Delete permanently' }));
        await waitFor(() =>
            expect(driver.execute).toHaveBeenCalledWith(
                7,
                [
                    { kind: 'DELETE', entryId: 'A', recursive: false },
                    { kind: 'DELETE', entryId: 'C', recursive: false },
                ],
                expect.any(Function),
            ),
        );
    });

    it('extends and shrinks keyboard ranges, moves focus without selection, and toggles with Space', async () => {
        const { view, rows, controller } = await setup();
        await fireEvent.click(rows[0]);
        await fireEvent.keyDown(rows[0], { key: 'ArrowDown', shiftKey: true });
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'B']);
        await fireEvent.keyDown(rows[1], { key: 'ArrowDown', shiftKey: true });
        await fireEvent.keyDown(rows[2], { key: 'ArrowUp', shiftKey: true });
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'B']);
        await fireEvent.keyDown(rows[1], { key: 'End', ctrlKey: true });
        expect(document.activeElement).toBe(rows[3]);
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'B']);
        await fireEvent.keyDown(rows[3], { key: ' ', ctrlKey: true });
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'B', 'D']);
        await fireEvent.keyDown(rows[3], { key: 'a', metaKey: true });
        expect(controller.selection).toHaveLength(4);
        await fireEvent.keyDown(rows[3], { key: 'Delete' });
        expect(view.getByRole('dialog').querySelectorAll('li')).toHaveLength(4);
    });

    it('keeps additive pointer ranges and selects only an unselected context-menu target', async () => {
        const { rows, controller, view } = await setup();
        await fireEvent.click(rows[0]);
        await fireEvent.click(rows[2], { metaKey: true });
        await fireEvent.click(rows[3], { shiftKey: true, metaKey: true });
        expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'C', 'D']);
        await fireEvent.contextMenu(rows[1]);
        expect(controller.selection.map((entry) => entry.id)).toEqual(['B']);
        expect(view.getByRole('menuitem', { name: 'Delete…' })).toBeTruthy();
    });

    it('does not enable directory creation for an ambiguous multi-selection', async () => {
        const { rows, view } = await setup();
        await fireEvent.click(rows[0]);
        await fireEvent.click(rows[1], { ctrlKey: true });
        expect((view.getByRole('button', { name: 'New directory...' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('clears the batch without moving keyboard focus and never silently deletes a protected subset', async () => {
        const { rows, view, controller } = await setup();
        await fireEvent.click(rows[0]);
        await fireEvent.click(rows[1], { ctrlKey: true });
        await fireEvent.click(view.getByRole('button', { name: 'Clear selection' }));
        expect(controller.selection).toEqual([]);
        await fireEvent.click(rows[0]);
        await fireEvent.keyDown(rows[0], { key: 'a', ctrlKey: true });
        controller.selection[1].filesystemMetadata = true;
        await waitFor(() =>
            expect(
                (view.getByRole('button', { name: 'Delete selected entries...' }) as HTMLButtonElement).disabled,
            ).toBe(true),
        );
        await fireEvent.keyDown(rows[0], { key: 'Delete' });
        expect(view.queryByRole('dialog')).toBeNull();
        await fireEvent.keyDown(rows[0], { key: 'Escape' });
        expect(controller.selection).toEqual([]);
        expect(rows[0].tabIndex).toBe(0);
    });
});
