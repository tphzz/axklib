import type { FilesystemMutationDriver } from '../../../lib/filesystem';
import { FilesystemWriteRejected } from '../../../lib/filesystem';
import { AxklibApiError } from '../../../lib/httpErrors';
import { CapacityWriteRejected } from '../../../lib/httpCapacityGate';
import type { ImageTransport } from '../../../lib/transport';
import type { JobController } from '../../jobs/actions';

interface Dependencies {
    transport: Pick<ImageTransport, 'startFilesystemEdits' | 'jobStatus' | 'cancelJob'>;
    imageFormat?: () => string | null;
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
        execute: (revision, edits, update, policy) =>
            dependencies.jobs.run(
                async () => {
                    active();
                    await dependencies.invalidateSession(sessionId);
                    active();
                    try {
                        return await dependencies.transport.startFilesystemEdits(sessionId, revision, edits, policy);
                    } catch (error) {
                        if (
                            error instanceof CapacityWriteRejected ||
                            (error instanceof AxklibApiError &&
                                error.status >= 400 &&
                                error.status < 500 &&
                                error.status !== 408)
                        )
                            throw new FilesystemWriteRejected(error.message);
                        throw error;
                    }
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
