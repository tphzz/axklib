import type { ApiJobSnapshot, AxklibHttpApiClient } from './httpApiClient';
import { AxklibApiError } from './httpErrors';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import type { ConversionInput, ConversionPlan, ConversionProfile, ConversionTransport } from './conversions';
import { documentTarget } from './httpNativeDocuments';

export class HttpConversions implements ConversionTransport {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        private readonly sessions: HttpImageSessions,
    ) {}

    async conversionProfiles(): Promise<ConversionProfile[]> {
        return (await this.client.request<{ profiles: ConversionProfile[] }>('GET', '/conversion-profiles')).profiles;
    }
    async prepareConversion(input: ConversionInput, signal?: AbortSignal): Promise<ConversionPlan> {
        signal?.throwIfAborted();
        const destination = this.sessions.get(input.destinationSessionId);
        const source = input.source.kind === 'DOCUMENT' ? this.sessions.get(input.source.sessionId) : null;
        const request = {
            profileId: input.profileId,
            source:
                source && input.source.kind === 'DOCUMENT'
                    ? {
                          kind: 'DOCUMENT',
                          imageId: source.remoteId,
                          documentId: input.source.documentId,
                          expectedRevision: input.source.expectedRevision,
                      }
                    : {
                          kind: 'SOURCES',
                          sources:
                              input.source.kind === 'SOURCES'
                                  ? input.source.sources.map((location) =>
                                        location.kind === 'server-directory'
                                            ? { kind: 'DIRECTORY', directory: location.reference }
                                            : location.kind === 'client-upload'
                                              ? { kind: 'UPLOAD', upload: location.reference }
                                              : { kind: 'FILE', file: location.reference },
                                    )
                                  : [],
                      },
            destination: {
                ...documentTarget(destination),
                expectedRevision: destination.revision,
                selection: input.selection,
            },
            options: input.options,
            fidelity: input.fidelity,
            capacityPolicy: input.capacityPolicy,
        };
        const submitted = await this.client.invoke<never>('conversions.prepare', request);
        if (!this.jobs.isJob(submitted)) throw new Error('Conversion preparation did not return a job');
        const completed = await this.jobs.wait(this.jobs.map(submitted).jobId, () => undefined, signal);
        if (completed.status !== 'completed' || !completed.result)
            throw new AxklibApiError(
                completed.errorCode ?? 'conversion_preparation_failed',
                completed.error ?? 'Conversion preparation failed',
                422,
            );
        const plan = completed.result as ConversionPlan;
        try {
            signal?.throwIfAborted();
            if (!plan.report) throw new Error('Conversion preparation returned no report');
            const count = Math.max(plan.report.actionCount, plan.report.issueCount);
            for (let offset = 200; offset < count; offset += 200) {
                const page = await this.conversionPlan(plan.planId, offset);
                signal?.throwIfAborted();
                if (!page.report || page.state !== 'READY') throw new Error('Conversion plan expired during review');
                plan.report.actions.push(...page.report.actions);
                plan.report.issues.push(...page.report.issues);
            }
            return plan;
        } catch (error) {
            await this.releaseConversion(plan.planId).catch(() => undefined);
            throw error;
        }
    }
    conversionPlan(planId: string, offset = 0): Promise<ConversionPlan> {
        return this.client.request('GET', `/conversion-plans/${encodeURIComponent(planId)}?offset=${offset}&limit=200`);
    }
    async releaseConversion(planId: string): Promise<void> {
        await this.client.request('DELETE', `/conversion-plans/${encodeURIComponent(planId)}`);
    }
    async conversionExecution(planId: string) {
        const plan = await this.conversionPlan(planId);
        if (!plan.jobId) return null;
        this.client.deviceWrites.acknowledge(`conversion:${planId}`, plan.jobId);
        return this.jobs.map(
            await this.client.request<ApiJobSnapshot>('GET', `/jobs/${encodeURIComponent(plan.jobId)}`),
        );
    }
    async executeConversion(plan: ConversionPlan, acknowledgedIssueIds: string[]) {
        const identity = `conversion:${plan.planId}`;
        const tracked = this.client.deviceWrites.begin(
            plan.report?.destination.imageId,
            'conversions.execute',
            identity,
        );
        try {
            const job = await this.client.request<ApiJobSnapshot>(
                'POST',
                `/conversion-plans/${encodeURIComponent(plan.planId)}/executions`,
                { acknowledgedIssueIds },
                { 'Idempotency-Key': identity },
            );
            this.client.deviceWrites.acknowledge(tracked, job.jobId);
            return this.jobs.map(job);
        } catch (error) {
            if (error instanceof AxklibApiError && error.status >= 400 && error.status < 500 && error.status !== 408) {
                if (tracked) this.client.deviceWrites.reconcile(tracked);
                throw error;
            }
            // Recover the accepted identity, never submit another write after a lost response.
            const accepted = await this.conversionExecution(plan.planId).catch(() => null);
            if (accepted) return accepted;
            throw error;
        }
    }
}
