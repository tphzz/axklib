import { diagnosticsEnabled, reportDiagnostic, type DiagnosticLevel } from '../../lib/diagnostics';
import type { JobState } from '../../lib/transport';

export class ImageOpenDiagnostics {
    private clientOpenRequestId = 0;
    private jobId: string | undefined;
    private lastProgressAt = 0;

    begin(requestId: number): void {
        this.clientOpenRequestId = requestId;
        this.jobId = undefined;
        this.lastProgressAt = 0;
    }
    report(event: string, fields: Readonly<Record<string, unknown>>, level: DiagnosticLevel = 'info'): void {
        reportDiagnostic(event, { clientOpenRequestId: this.clientOpenRequestId, jobId: this.jobId, ...fields }, level);
    }
    progress(job: JobState, elapsedMs: number): void {
        this.jobId = job.serverJobId;
        if (!diagnosticsEnabled() || (performance.now() - this.lastProgressAt < 200 && job.status !== 'completed'))
            return;
        this.lastProgressAt = performance.now();
        this.report(
            'image_open_progress',
            {
                clientJobId: job.jobId,
                status: job.status,
                phase: job.progress?.phase,
                completed: job.progress?.completed,
                total: job.progress?.total,
                label: job.progress?.label,
                elapsedMs,
            },
            'debug',
        );
    }
}
