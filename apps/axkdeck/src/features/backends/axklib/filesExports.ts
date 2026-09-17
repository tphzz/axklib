import type { FilesystemExportDriver } from '../../../lib/filesystemExport';
import type { ImageTransport } from '../../../lib/transport';
import type { JobController } from '../../jobs/actions';

interface Dependencies {
    transport: Pick<ImageTransport, 'inspectFilesystemExport' | 'startFilesystemExport' | 'jobStatus' | 'cancelJob'>;
    jobs: JobController;
    sessionId: () => number | null;
}

export function bindFilesystemExports(dependencies: Dependencies, sessionId: number): FilesystemExportDriver {
    const active = (): void => {
        if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
    };
    return {
        inspect: async (revision, entryIds, layout = 'SELECTED_ENTRIES') => {
            active();
            const result = await dependencies.transport.inspectFilesystemExport(sessionId, revision, entryIds, layout);
            active();
            return result;
        },
        execute: (revision, entryIds, destination, update, layout = 'SELECTED_ENTRIES') =>
            dependencies.jobs.run(
                async () => {
                    active();
                    return dependencies.transport.startFilesystemExport(
                        sessionId,
                        revision,
                        entryIds,
                        destination,
                        layout,
                    );
                },
                update,
                update,
            ),
        observe: (jobId, update) =>
            dependencies.jobs.run(() => dependencies.transport.jobStatus(jobId), update, update),
        cancel: (jobId) => dependencies.transport.cancelJob(jobId),
    };
}
