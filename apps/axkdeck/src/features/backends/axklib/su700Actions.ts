import type { FilesystemImportActions } from '../../../lib/filesystemImport';
import type { Su700Request } from '../../../lib/su700Import';
import type { JobState, ImageTransport } from '../../../lib/transport';
import type { JobController } from '../../jobs/actions';

export interface Su700ImportActions {
    run(request: Su700Request, update: (job: JobState) => void): Promise<JobState>;
    observe(jobId: number, update: (job: JobState) => void): Promise<JobState>;
    cancel(jobId: number): Promise<void>;
}
export type AxklibFilesystemImports = FilesystemImportActions & { su700?: Su700ImportActions };

export function bindSu700Imports(
    dependencies: {
        transport: Pick<ImageTransport, 'startSu700Import' | 'cancelJob' | 'jobStatus'>;
        jobs: JobController;
        sessionId: () => number | null;
        invalidateSession: (id: number) => Promise<void>;
    },
    sessionId: number,
): Su700ImportActions {
    const active = () => {
        if (dependencies.sessionId() !== sessionId) throw new Error('The reviewed image is no longer open.');
    };
    return {
        run: (request, update) =>
            dependencies.jobs.run(
                async () => {
                    active();
                    if (request.expectedSource) await dependencies.invalidateSession(sessionId);
                    active();
                    const job = await dependencies.transport.startSu700Import(request);
                    if (dependencies.sessionId() !== sessionId) {
                        await dependencies.transport.cancelJob(job.jobId).catch(() => undefined);
                        active();
                    }
                    return job;
                },
                update,
                update,
            ),
        observe: (id, update) => dependencies.jobs.run(() => dependencies.transport.jobStatus(id), update, update),
        cancel: (id) => dependencies.transport.cancelJob(id),
    };
}
