import type { FilesystemExportActions } from '../../../lib/filesystemExport';
import { shouldUseDirectComputerFileOperations } from '../../../lib/fileOperationRouting';
import {
    selectLocalDirectoryExportDestination,
    saveRetainedDirectoryExport,
    cancelRetainedDirectoryExport,
} from '../../../lib/nativeDirectoryExports';
import type { DirectoryRef } from '../../../lib/storageLocations';
import type { ImageTransport } from '../../../lib/transport';
import type { PickerController } from '../../dialogs/picker';
import { bindFilesystemExports } from './filesExports';
import { nativeFilesDrag } from '../../../lib/nativeFilesDrag';

type Dependencies = Parameters<typeof bindFilesystemExports>[0] & {
    transport: Pick<ImageTransport, 'connectionMode' | 'deleteRetainedPackage'>;
    picker: PickerController;
    isDesktop: boolean;
};

export function bindFilesystemExportActions(dependencies: Dependencies, sessionId: number): FilesystemExportActions {
    const directComputer = shouldUseDirectComputerFileOperations(
        dependencies.isDesktop,
        dependencies.transport.connectionMode,
    );
    let lastDirectory: DirectoryRef | null = null;
    return {
        ...bindFilesystemExports(dependencies, sessionId),
        directComputer,
        desktop: dependencies.isDesktop,
        drag: dependencies.isDesktop ? nativeFilesDrag : undefined,
        chooseDestination: async (route, suggestedName) => {
            if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
            if (route === 'computer') {
                if (!dependencies.isDesktop) throw new Error('This destination requires the desktop application.');
                const candidate = await selectLocalDirectoryExportDestination(suggestedName, 'Files');
                if (!candidate) return null;
                return {
                    destination: { kind: 'DOWNLOAD', directoryName: candidate.directoryName },
                    cancelPublication: () => cancelRetainedDirectoryExport(candidate.candidateId),
                    publish: async (result) => {
                        if (result.destination !== 'DOWNLOAD' || !result.download)
                            throw new Error('The export did not provide a retained download.');
                        await saveRetainedDirectoryExport(
                            candidate.candidateId,
                            result.download.contentPath,
                            result.download.sizeBytes,
                        );
                    },
                };
            }
            if (directComputer) throw new Error('This export uses the direct-computer destination.');
            const selection = await dependencies.picker.chooseLocation(
                'save-directory',
                'Export files',
                [],
                suggestedName,
                {
                    parentDialog: 'filesystem-export',
                    requireWritableDirectory: true,
                    initialDirectory: lastDirectory,
                    ondirectorychange: (directory) => {
                        lastDirectory = directory;
                    },
                },
            );
            if (selection?.kind !== 'server-directory') return null;
            return {
                destination: { kind: 'WORKSPACE', output: selection.reference },
                publish: async (result) => {
                    if (result.destination !== 'WORKSPACE')
                        throw new Error('The export did not confirm the workspace destination.');
                },
            };
        },
        release: async (result) => {
            if (result.download) await dependencies.transport.deleteRetainedPackage(result.download);
        },
    };
}
