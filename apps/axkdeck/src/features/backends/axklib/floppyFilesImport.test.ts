import { describe, expect, it, vi } from 'vitest';
import { openASeriesFloppy } from './floppyFilesImport';
import type { FloppyImportWorkflow } from '../../import/floppyWorkflow.svelte';
import type { FilesController } from '../../files/controller.svelte';
import type { FilesystemEntry } from '../../../lib/filesystem';
import { browserUploadSource } from '../../../lib/clientUploadSource';
import type { ClientFilesystemImportEntry } from '../../../lib/filesystemImport';

function setup() {
    const volume = { id: 'semantic-volume', name: 'Not a filename' };
    const parent = { id: 'directory', parentId: 'root', contentScopeId: volume.id } as FilesystemEntry;
    const file = { id: 'file', parentId: parent.id, kind: 'file' } as FilesystemEntry;
    const workflow = {
        available: true,
        request: null,
        destinations: () => ({ volumeItems: [volume], partitionItems: [] }),
        open: vi.fn(),
        requestDroppedFiles: vi.fn().mockResolvedValue(undefined),
    };
    const controller = { revision: 1, selected: null, root: parent, lookup: vi.fn().mockResolvedValue(parent) };
    const run = (entries?: ClientFilesystemImportEntry[]) =>
        openASeriesFloppy(
            workflow as unknown as FloppyImportWorkflow,
            controller as unknown as FilesController,
            entries,
            file,
        );
    return { run, workflow, controller, volume };
}
const source = browserUploadSource(new File(['disk'], 'floppy.img'));
const disk: ClientFilesystemImportEntry = { relativePath: ['floppy.img'], directory: false, source };
describe('A-series Files floppy destination', () => {
    it('resolves a selected file through its parent identity for drops and menu commands', async () => {
        const { run, workflow, volume } = setup();
        expect(await run([disk])).toBe(true);
        expect(workflow.requestDroppedFiles).toHaveBeenCalledWith([source], volume);
        expect(await run()).toBe(true);
        expect(workflow.open).toHaveBeenCalledWith(volume);
    });
    it('leaves ordinary raw files to their importer but rejects mixed and nested image drops', async () => {
        const { run, workflow } = setup();
        const raw = { ...disk, relativePath: ['file.dat'] };
        expect(await run([raw])).toBe(false);
        await expect(run([disk, raw])).rejects.toThrow('separately');
        await expect(run([{ ...disk, relativePath: ['folder', 'disk.img'] }])).rejects.toThrow('separately');
        expect(workflow.open).not.toHaveBeenCalled();
    });
    it('rejects a revision change during ancestry lookup rather than opening a stale destination', async () => {
        const { run, controller, workflow } = setup();
        controller.lookup.mockImplementationOnce(async () => {
            controller.revision++;
            return controller.root;
        });
        await expect(run([disk])).rejects.toThrow('destination changed');
        expect(workflow.requestDroppedFiles).not.toHaveBeenCalled();
    });
});
