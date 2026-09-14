import type { FloppyImportWorkflow } from '../../import/floppyWorkflow.svelte';
import type { FilesController } from '../../files/controller.svelte';
import type { ClientFilesystemImportEntry } from '../../../lib/filesystemImport';
import type { FilesystemEntry } from '../../../lib/filesystem';

export async function openASeriesFloppy(
    workflow: FloppyImportWorkflow,
    controller: FilesController,
    entries?: ClientFilesystemImportEntry[],
    target?: FilesystemEntry,
): Promise<boolean> {
    if (!workflow.available || workflow.request) return true;
    if (entries) {
        const candidates = entries.filter(
            (entry) => !entry.directory && /\.(img|ima)$/i.test(entry.relativePath.at(-1) ?? ''),
        );
        if (!candidates.length) return false;
        if (candidates.length !== entries.length || entries.some((entry) => entry.relativePath.length !== 1))
            throw new Error('Drop floppy images separately from other files or directories.');
    }
    const revision = controller.revision;
    const destinations = workflow.destinations();
    const scopes = [...destinations.volumeItems, ...destinations.partitionItems];
    let entry = target ?? controller.selected ?? controller.root;
    const visited = new Set<string>();
    while (entry && !visited.has(entry.id)) {
        visited.add(entry.id);
        const item = scopes.find((scope) => scope.id === entry!.contentScopeId);
        if (item) {
            if (controller.revision !== revision) throw new Error('The destination changed. Choose it again.');
            if (entries)
                await workflow.requestDroppedFiles(
                    entries.flatMap((value) => (value.directory ? [] : [value.source])),
                    item,
                );
            else workflow.open(item);
            return true;
        }
        entry = entry.parentId ? await controller.lookup({ entryId: entry.parentId }) : null;
    }
    throw new Error('No corresponding A-series partition or volume was found for this destination.');
}
