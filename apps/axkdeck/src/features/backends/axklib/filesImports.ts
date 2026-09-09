import type { FilesystemImportDriver } from '../../../lib/filesystemImport';
import type { ImageTransport, JobState } from '../../../lib/transport';
import type { JobController } from '../../jobs/actions';

interface Dependencies {
    transport: Pick<
        ImageTransport,
        'startFilesystemInputInspection' | 'startFilesystemImportInspection' | 'jobStatus' | 'cancelJob'
    >;
    jobs: JobController;
    sessionId: () => number | null;
}

export function bindFilesystemImports(dependencies: Dependencies, sessionId: number): FilesystemImportDriver {
    const active = (): void => {
        if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
    };
    const inspect = async (start: () => Promise<JobState>, update: (job: JobState) => void): Promise<JobState> => {
        const result = await dependencies.jobs.run(
            async () => {
                active();
                const job = await start();
                if (dependencies.sessionId() !== sessionId) {
                    await dependencies.transport.cancelJob(job.jobId).catch(() => undefined);
                    active();
                }
                return job;
            },
            update,
            update,
        );
        active();
        return result;
    };
    return {
        inspectInputs: (inputs, update) =>
            inspect(() => dependencies.transport.startFilesystemInputInspection(inputs), update),
        inspectDestination: (revision, parentEntryId, entries, update) =>
            inspect(
                () =>
                    dependencies.transport.startFilesystemImportInspection(sessionId, revision, parentEntryId, entries),
                update,
            ),
        observe: (jobId, update) =>
            dependencies.jobs.run(() => dependencies.transport.jobStatus(jobId), update, update),
        cancel: (jobId) => dependencies.transport.cancelJob(jobId),
    };
}
