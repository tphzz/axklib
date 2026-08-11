import type { ApiJobEvent, ApiJobSnapshot, EventConnection } from './httpApiClient';
import type { AxklibHttpApiClient } from './httpApiClient';
import { AxklibApiError } from './httpErrors';
import type { JobState } from './transport';

const maximumRetainedTerminalJobs = 128;
const cancellationConcurrency = 4;

export class HttpJobController {
    private readonly activeJobs = new Map<number, string>();
    private readonly terminalJobs = new Map<number, string>();
    private nextJobId = 1;

    constructor(private readonly client: AxklibHttpApiClient) {}

    isJob(value: unknown): value is ApiJobSnapshot {
        return typeof value === 'object' && value !== null && 'jobId' in value && 'state' in value;
    }

    map(job: ApiJobSnapshot, existingId?: number): JobState {
        const jobId = existingId ?? this.nextJobId++;
        const progress = job.progress as
            { phase?: string; completed?: number; total?: number | null; message?: string } | undefined;
        const error = job.error as { code?: string; message?: string; context?: unknown } | undefined;
        const mapped: JobState = {
            jobId,
            kind: job.operationId,
            status: job.state.toLocaleLowerCase() as JobState['status'],
            progress: progress
                ? {
                      phase: 0,
                      completed: progress.completed ?? 0,
                      total: progress.total ?? undefined,
                      label: progress.message ?? progress.phase ?? job.state,
                  }
                : undefined,
            result: job.result,
            error: error?.message,
            errorCode: error?.code,
            errorContext: error?.context,
        };
        this.track(jobId, job.jobId, this.terminal(mapped));
        return mapped;
    }

    async status(jobId: number): Promise<JobState> {
        const remoteId = this.remoteId(jobId);
        if (!remoteId) throw new Error('Job is closed or unknown');
        const job = await this.client.request<ApiJobSnapshot>('GET', `/jobs/${encodeURIComponent(remoteId)}`);
        return this.map(job, jobId);
    }

    wait(jobId: number, onUpdate: (job: JobState) => void, signal?: AbortSignal): Promise<JobState> {
        const remoteId = this.remoteId(jobId);
        if (!remoteId) return Promise.reject(new Error('Job is closed or unknown'));

        return new Promise((resolve, reject) => {
            let afterSequence = 0;
            let connection: EventConnection | undefined;
            let reconnectAttempts = 0;
            let stableConnectionTimer: ReturnType<typeof setTimeout> | undefined;
            let settled = false;
            let cancellationRequested = false;
            let work = Promise.resolve();

            const clearStableConnectionTimer = (): void => {
                if (stableConnectionTimer !== undefined) clearTimeout(stableConnectionTimer);
                stableConnectionTimer = undefined;
            };
            const markConnectionHealthy = (): void => {
                reconnectAttempts = 0;
                clearStableConnectionTimer();
            };
            const finish = (job: JobState): void => {
                if (settled) return;
                settled = true;
                clearStableConnectionTimer();
                signal?.removeEventListener('abort', handleAbort);
                connection?.close();
                resolve(job);
            };
            const fail = (reason: unknown): void => {
                if (settled) return;
                settled = true;
                clearStableConnectionTimer();
                signal?.removeEventListener('abort', handleAbort);
                connection?.close();
                reject(reason);
            };
            const publishSnapshot = (snapshot: ApiJobSnapshot): JobState => {
                afterSequence = Math.max(afterSequence, snapshot.latestSequence ?? afterSequence);
                const mapped = this.map(snapshot, jobId);
                onUpdate(mapped);
                if (this.terminal(mapped)) finish(mapped);
                return mapped;
            };
            const publishEvent = (event: ApiJobEvent): void => {
                afterSequence = event.sequence;
                const mapped = this.mapEvent(event, jobId);
                onUpdate(mapped);
            };
            const readSnapshot = async (): Promise<void> => {
                const snapshot = await this.client.request<ApiJobSnapshot>(
                    'GET',
                    `/jobs/${encodeURIComponent(remoteId)}`,
                );
                publishSnapshot(snapshot);
            };
            const reconcile = async (): Promise<void> => {
                if (settled) return;
                let replay: { events: ApiJobEvent[] };
                try {
                    replay = await this.client.replayJobEvents(remoteId, afterSequence);
                } catch (reason) {
                    if (!(reason instanceof AxklibApiError) || reason.code !== 'job_event_replay_expired') throw reason;
                    await readSnapshot();
                    return;
                }
                for (const event of replay.events) {
                    if (event.sequence <= afterSequence) continue;
                    if (event.sequence !== afterSequence + 1) {
                        throw new Error(`Job event replay is discontinuous after sequence ${afterSequence}`);
                    }
                    publishEvent(event);
                }
                await readSnapshot();
            };
            const enqueue = (task: () => Promise<void>): void => {
                work = work.then(task).catch(fail);
            };
            const handleAbort = (): void => {
                if (settled || cancellationRequested) return;
                cancellationRequested = true;
                enqueue(async () => {
                    try {
                        await this.cancel(jobId);
                    } catch (reason) {
                        await reconcile();
                        if (!settled) throw reason;
                        return;
                    }
                    await reconcile();
                });
            };
            const handleEvent = (event: ApiJobEvent): void => {
                if (settled || event.jobId !== remoteId || event.sequence <= afterSequence) return;
                markConnectionHealthy();
                if (event.sequence !== afterSequence + 1) {
                    enqueue(reconcile);
                    return;
                }
                publishEvent(event);
                if (this.terminalState(event.state)) {
                    enqueue(readSnapshot);
                }
            };
            const connect = async (): Promise<void> => {
                if (settled) return;
                try {
                    connection = await this.client.connectEvents(handleEvent, () => {
                        connection = undefined;
                        clearStableConnectionTimer();
                        if (settled) return;
                        enqueue(async () => {
                            reconnectAttempts += 1;
                            await reconcile();
                            if (settled) return;
                            if (reconnectAttempts > 6) {
                                fail(new Error('Lost the axklib-server event connection'));
                                return;
                            }
                            const delay = Math.min(2_000, 100 * 2 ** (reconnectAttempts - 1));
                            setTimeout(() => void connect(), delay);
                        });
                    });
                    await connection.opened;
                    clearStableConnectionTimer();
                    stableConnectionTimer = setTimeout(markConnectionHealthy, 10_000);
                    enqueue(reconcile);
                } catch (reason) {
                    reconnectAttempts += 1;
                    await reconcile();
                    if (settled) return;
                    if (reconnectAttempts > 6) {
                        fail(reason);
                        return;
                    }
                    const delay = Math.min(2_000, 100 * 2 ** (reconnectAttempts - 1));
                    setTimeout(() => void connect(), delay);
                }
            };

            enqueue(async () => {
                await reconcile();
                if (!settled) await connect();
            });
            signal?.addEventListener('abort', handleAbort, { once: true });
            if (signal?.aborted) handleAbort();
        });
    }

    async cancel(jobId?: number): Promise<void> {
        if (jobId !== undefined) {
            const remoteId = this.activeJobs.get(jobId);
            if (remoteId) await this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`);
            return;
        }
        const remoteIds = [...new Set(this.activeJobs.values())];
        for (let offset = 0; offset < remoteIds.length; offset += cancellationConcurrency) {
            const batch = remoteIds.slice(offset, offset + cancellationConcurrency);
            await Promise.all(
                batch.map((remoteId) => this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`)),
            );
        }
    }

    private mapEvent(event: ApiJobEvent, jobId: number): JobState {
        const progress = event.progress;
        const mapped: JobState = {
            jobId,
            kind: event.operationId,
            status: event.state.toLocaleLowerCase() as JobState['status'],
            progress: progress
                ? {
                      phase: 0,
                      completed: progress.completed,
                      total: progress.total ?? undefined,
                      label: progress.message || progress.phase || event.state,
                  }
                : undefined,
        };
        this.track(jobId, event.jobId, this.terminal(mapped));
        return mapped;
    }

    private remoteId(jobId: number): string | undefined {
        return this.activeJobs.get(jobId) ?? this.terminalJobs.get(jobId);
    }

    private track(jobId: number, remoteId: string, terminal: boolean): void {
        if (!terminal) {
            this.terminalJobs.delete(jobId);
            this.activeJobs.set(jobId, remoteId);
            return;
        }
        this.activeJobs.delete(jobId);
        this.terminalJobs.delete(jobId);
        this.terminalJobs.set(jobId, remoteId);
        while (this.terminalJobs.size > maximumRetainedTerminalJobs) {
            const oldest = this.terminalJobs.keys().next().value as number | undefined;
            if (oldest === undefined) break;
            this.terminalJobs.delete(oldest);
        }
    }

    private terminal(job: JobState): boolean {
        return job.status === 'completed' || job.status === 'failed' || job.status === 'cancelled';
    }

    private terminalState(state: string): boolean {
        return state === 'COMPLETED' || state === 'FAILED' || state === 'CANCELLED';
    }
}
