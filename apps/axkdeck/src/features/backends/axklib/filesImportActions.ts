import type { FilesystemImportActions } from '../../../lib/filesystemImport';
import type { FilesystemEditSource, FilesystemImageInspection } from '../../../lib/filesystem';
import type { DirectoryRef, InputFileLocation } from '../../../lib/storageLocations';
import type { ImageTransport } from '../../../lib/transport';
import type { PickerController } from '../../dialogs/picker';
import { bindFilesystemImports } from './filesImports';
import { readFilesystemImportTree } from './filesImportTree';

type Dependencies = Parameters<typeof bindFilesystemImports>[0] & {
    transport: Pick<
        ImageTransport,
        | 'supportsClientUploads'
        | 'uploadClientFile'
        | 'releaseClientUpload'
        | 'sandboxDirectory'
        | 'startFilesystemImageInspection'
        | 'releaseFilesystemImageInspection'
    >;
    picker: PickerController;
};

export function bindFilesystemImportActions(dependencies: Dependencies, sessionId: number): FilesystemImportActions {
    let lastDirectory: DirectoryRef | null = null;
    const active = (): void => {
        if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
    };
    const release = async (sources: FilesystemEditSource[]): Promise<void> => {
        for (const token of new Set(
            sources.flatMap((source) => (source.kind === 'image-entry' ? [source.reference.inspectionToken] : [])),
        ))
            await dependencies.transport.releaseFilesystemImageInspection(token);
        for (const source of sources)
            if (source.kind === 'client-upload')
                await dependencies.transport.releaseClientUpload(source).catch(() => undefined);
    };
    return {
        ...bindFilesystemImports(dependencies, sessionId),
        images: {
            inspect: async (source, update) => {
                active();
                const job = await dependencies.jobs.run(
                    () => dependencies.transport.startFilesystemImageInspection(source),
                    update,
                    update,
                );
                if (dependencies.sessionId() !== sessionId) {
                    const token = (job.result as FilesystemImageInspection | undefined)?.inspectionToken;
                    if (token) await dependencies.transport.releaseFilesystemImageInspection(token);
                    active();
                }
                return job;
            },
            release: (token) => dependencies.transport.releaseFilesystemImageInspection(token),
        },
        supportsClientUploads: dependencies.transport.supportsClientUploads,
        chooseDirectory: async (signal, progress) => {
            active();
            signal.throwIfAborted();
            const directory = await dependencies.picker.chooseLocation('directory', 'Import from disk', [], '', {
                parentDialog: 'filesystem-import',
                initialDirectory: lastDirectory,
                ondirectorychange: (value) => {
                    lastDirectory = value;
                },
            });
            active();
            signal.throwIfAborted();
            if (!directory) return null;
            if (directory.kind !== 'server-directory') throw new Error('Choose a source directory.');
            progress('Reading source directory');
            return readFilesystemImportTree(
                directory.reference,
                (reference, cursor) => dependencies.transport.sandboxDirectory(reference, cursor),
                signal,
                active,
                progress,
            );
        },
        chooseFiles: async (title = 'Add files') => {
            active();
            const files = await dependencies.picker.chooseFiles(title, [], {
                parentDialog: 'filesystem-import',
                initialDirectory: lastDirectory,
                ondirectorychange: (directory) => {
                    lastDirectory = directory;
                },
            });
            active();
            return files;
        },
        upload: async (files, signal, progress) => {
            active();
            if (!dependencies.transport.supportsClientUploads) throw new Error('Client uploads are unavailable.');
            if (!files.length || files.length > 10000) throw new Error('Choose between 1 and 10000 files.');
            if (files.some((file) => !Number.isSafeInteger(file.size) || file.size < 0 || file.size > 0xffffffff))
                throw new Error('Each file must fit the filesystem 32-bit size field.');
            const sources: InputFileLocation[] = [];
            try {
                for (const [index, file] of files.entries()) {
                    active();
                    signal.throwIfAborted();
                    sources.push(
                        await dependencies.transport.uploadClientFile(
                            file,
                            'FILE',
                            (sent, total) => {
                                progress(
                                    `Uploading ${index + 1} of ${files.length}: ${total ? Math.round((sent * 100) / total) : 100}%`,
                                );
                            },
                            signal,
                        ),
                    );
                    active();
                    signal.throwIfAborted();
                }
                return sources;
            } catch (error) {
                await release(sources);
                throw error;
            }
        },
        release,
    };
}
