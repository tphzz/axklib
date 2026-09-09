import type { FilesystemMutationDriver } from '../../../lib/filesystem';
import type { ImageTransport } from '../../../lib/transport';
import type { JobController } from '../../jobs/actions';

interface Dependencies {
    transport: Pick<ImageTransport, 'startFilesystemEdits' | 'jobStatus' | 'cancelJob'>;
    jobs: JobController;
    sessionId: () => number | null;
    invalidateSession: (sessionId: number) => Promise<void>;
    refreshSession: () => Promise<void>;
}

export function bindFilesystemMutations(dependencies: Dependencies, sessionId: number): FilesystemMutationDriver {
    const active = (): void => {
        if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
    };
    return {
        execute: (revision, edits, update) =>
            dependencies.jobs.run(
                async () => {
                    active();
                    await dependencies.invalidateSession(sessionId);
                    active();
                    return dependencies.transport.startFilesystemEdits(sessionId, revision, edits);
                },
                update,
                update,
            ),
        observe: (jobId, update) =>
            dependencies.jobs.run(() => dependencies.transport.jobStatus(jobId), update, update),
        cancel: (jobId) => dependencies.transport.cancelJob(jobId),
        refresh: async () => {
            active();
            await dependencies.refreshSession();
            active();
        },
    };
}
