import { cleanup, fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, expect, it, vi } from 'vitest';
import FilesView from './FilesView.svelte';
import { FilesController } from './controller.svelte';
import { filesystemEntry } from '../../lib/testing/filesystem';
import type { FilesystemExportActions, FilesystemExportResult } from '../../lib/filesystemExport';

afterEach(cleanup);

function pointer(type: string, x: number, buttons = 1): MouseEvent {
    const event = new MouseEvent(type, { clientX: x, clientY: 10, button: 0, buttons, bubbles: true });
    Object.defineProperty(event, 'pointerId', { value: 1 });
    return event;
}

it('keeps clicks selection-only, starts after threshold, preserves the batch and does not open a picker', async () => {
    const root = filesystemEntry({ id: 'root', kind: 'root', path: '', parentId: null });
    const entries = ['A', 'B'].map((id) => filesystemEntry({ id, name: id, parentId: 'root', kind: 'file' }));
    const controller = new FilesController({
        inspect: async (query = {}) => ({
            revision: 7,
            available: true,
            deviceView: null,
            filesystemName: 'Test',
            items: query.parentId ? entries : [root],
            totalCount: query.parentId ? 2 : 1,
            rootCapabilities: [],
        }),
    });
    await controller.initialize();
    const result: FilesystemExportResult = {
        rootDirectory: null,
        imageId: 'image',
        revision: 7,
        entries: [{ entryId: 'A', relativePath: ['A'], sourcePath: '/A', sizeBytes: 3, directory: false }],
        notices: [],
        totalBytes: 3,
        destination: 'DOWNLOAD',
        output: null,
        download: {
            archiveId: 'test',
            filename: 'Files.tar',
            contentPath: '/api/v1/download-archives/test/content',
            sizeBytes: 2048,
            expiresInSeconds: 300,
        },
    };
    const driver = {
        directComputer: true,
        desktop: true,
        inspect: vi.fn().mockResolvedValue(result),
        execute: vi.fn().mockResolvedValue({ jobId: 1, status: 'completed', result }),
        observe: vi.fn(),
        cancel: vi.fn(),
        release: vi.fn().mockResolvedValue(undefined),
        chooseDestination: vi.fn(),
        drag: {
            reserve: vi.fn().mockResolvedValue('ticket'),
            prepare: vi.fn().mockResolvedValue(undefined),
            start: vi.fn().mockResolvedValue(undefined),
            cancel: vi.fn().mockResolvedValue(undefined),
        },
    } satisfies FilesystemExportActions;
    const view = render(FilesView, { controller, exports: driver });
    const rows = view.getAllByRole('row');
    await fireEvent(rows[0], pointer('pointerdown', 10));
    await fireEvent(window, pointer('pointerup', 10, 0));
    await fireEvent.click(rows[0]);
    await fireEvent.click(rows[1], { ctrlKey: true });
    expect(driver.inspect).not.toHaveBeenCalled();
    await fireEvent(rows[0], pointer('pointerdown', 10));
    await fireEvent(window, pointer('pointermove', 13));
    expect(driver.inspect).not.toHaveBeenCalled();
    await fireEvent(window, pointer('pointermove', 30));
    expect(driver.inspect).not.toHaveBeenCalled();
    await fireEvent(window, pointer('pointermove', window.innerWidth + 10));
    await waitFor(() => expect(driver.drag.start).toHaveBeenCalledOnce());
    expect(driver.execute).toHaveBeenCalledWith(
        7,
        ['A', 'B'],
        { kind: 'DOWNLOAD', directoryName: 'Files' },
        expect.any(Function),
    );
    await fireEvent(window, pointer('pointerup', 30, 0));
    await fireEvent.click(rows[0]);
    expect(controller.selection.map((entry) => entry.id)).toEqual(['A', 'B']);
    expect(driver.chooseDestination).not.toHaveBeenCalled();
    expect(view.queryByRole('dialog')).toBeNull();
});
