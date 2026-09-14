import { bindFilesystemMutations } from './filesMutations';
import { bindFilesystemExportActions } from './filesExportActions';
import { bindFilesystemImportActions } from './filesImportActions';
import { bindSu700Imports } from './su700Actions';
import type { FloppyImportWorkflow } from '../../import/floppyWorkflow.svelte';

type Dependencies = Parameters<typeof bindFilesystemMutations>[0] &
    Parameters<typeof bindFilesystemExportActions>[0] &
    Parameters<typeof bindFilesystemImportActions>[0] &
    Parameters<typeof bindSu700Imports>[0] & { floppy?: () => FloppyImportWorkflow };

export function createFilesystemBindings(dependencies: Dependencies) {
    return {
        filesMutations: (sessionId: number) => bindFilesystemMutations(dependencies, sessionId),
        filesExports: (sessionId: number) => bindFilesystemExportActions(dependencies, sessionId),
        filesImports: (sessionId: number) => ({
            ...bindFilesystemImportActions(dependencies, sessionId),
            su700: bindSu700Imports(dependencies, sessionId),
            floppy: dependencies.floppy?.(),
        }),
    };
}
