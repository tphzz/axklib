import type { ApiJobSnapshot, AxklibHttpApiClient } from './httpApiClient';
import { AxklibApiError } from './httpErrors';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import type {
    CompanionSourceInput,
    CompanionSourceResolution,
    ConversionInput,
    ConversionInspectionInput,
    ConversionInspection,
    ConversionSource,
    ConversionPlan,
    ConversionProfile,
    ConversionTransport,
} from './conversions';
import type { InputFileLocation } from './storageLocations';
import { documentTarget } from './httpNativeDocuments';
import { readDirectoryArchive } from './httpDownloads';
import type { RetainedDownload } from './transport';

export class HttpConversions implements ConversionTransport {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        private readonly sessions: HttpImageSessions,
    ) {}

    async resolveCompanionSources(
        input: CompanionSourceInput,
        signal?: AbortSignal,
    ): Promise<CompanionSourceResolution> {
        signal?.throwIfAborted();
        const wire = (source: InputFileLocation) =>
            source.kind === 'client-upload'
                ? { kind: 'UPLOAD', upload: source.reference }
                : { kind: 'FILE', file: source.reference };
        const submitted = await this.client.invoke<never>('images.companionSources.resolve', {
            ...(input.formatId ? { formatId: input.formatId } : {}),
            sources: input.sources.map(wire),
            ...(input.candidates ? { candidates: input.candidates.map(wire) } : {}),
            ...(input.exclusions ? { exclusions: input.exclusions.map(wire) } : {}),
            ...(input.searchNearby !== undefined ? { searchNearby: input.searchNearby } : {}),
        });
        if (!this.jobs.isJob(submitted)) throw new Error('Companion resolution did not return a job');
        const completed = await this.jobs.wait(this.jobs.map(submitted).jobId, () => undefined, signal);
        signal?.throwIfAborted();
        if (completed.status !== 'completed' || !completed.result)
            throw new AxklibApiError(
                completed.errorCode ?? 'companion_resolution_failed',
                completed.error ?? 'Companion resolution failed',
                422,
            );
        return completed.result as CompanionSourceResolution;
    }

    async conversionProfiles(): Promise<ConversionProfile[]> {
        return (await this.client.request<{ profiles: ConversionProfile[] }>('GET', '/conversion-profiles')).profiles;
    }
    async readConversionDownload(download: RetainedDownload): Promise<Blob> {
        return (await readDirectoryArchive(this.client, download)).blob;
    }
    private wireSource(source: ConversionSource) {
        if (source.kind === 'DOCUMENT')
            return {
                kind: source.kind,
                imageId: this.sessions.get(source.sessionId).remoteId,
                documentId: source.documentId,
                expectedRevision: source.expectedRevision,
            };
        return {
            kind: source.kind,
            sources: source.sources.map((location) =>
                location.kind === 'server-directory'
                    ? { kind: 'DIRECTORY', directory: location.reference }
                    : location.kind === 'client-upload'
                      ? { kind: 'UPLOAD', upload: location.reference }
                      : { kind: 'FILE', file: location.reference },
            ),
        };
    }
    async inspectConversion(input: ConversionInspectionInput, signal?: AbortSignal): Promise<ConversionInspection> {
        signal?.throwIfAborted();
        const submitted = await this.client.invoke<never>('conversions.inspect', {
            ...input,
            source: this.wireSource(input.source),
        });
        if (!this.jobs.isJob(submitted)) throw new Error('Conversion inspection did not return a job');
        const completed = await this.jobs.wait(this.jobs.map(submitted).jobId, () => undefined, signal);
        signal?.throwIfAborted();
        if (completed.status !== 'completed' || !completed.result)
            throw new AxklibApiError(
                completed.errorCode ?? 'conversion_inspection_failed',
                completed.error ?? 'Conversion inspection failed',
                422,
            );
        return completed.result as ConversionInspection;
    }
    async prepareConversion(input: ConversionInput, signal?: AbortSignal): Promise<ConversionPlan> {
        signal?.throwIfAborted();
        let destination;
        if ('destinationSessionId' in input) {
            const session = this.sessions.get(input.destinationSessionId);
            destination = {
                ...documentTarget(session),
                expectedRevision: session.revision,
                selection: input.selection,
            };
        } else {
            destination = input.destination;
        }
        const request = {
            profileId: input.profileId,
            source: this.wireSource(input.source),
            destination,
            options: input.options,
            fidelity: input.fidelity,
            ...('capacityPolicy' in input ? { capacityPolicy: input.capacityPolicy } : {}),
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
            const count = Math.max(plan.report.actionCount, plan.report.issueCount, plan.report.sourceCount ?? 0);
            for (let offset = 200; offset < count; offset += 200) {
                const page = await this.conversionPlan(plan.planId, offset);
                signal?.throwIfAborted();
                if (!page.report || page.state !== 'READY') throw new Error('Conversion plan expired during review');
                plan.report.actions.push(...page.report.actions);
                plan.report.issues.push(...page.report.issues);
                if (page.report.sourceObjects) plan.report.sourceObjects?.push(...page.report.sourceObjects);
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
            plan.report && 'imageId' in plan.report.destination ? plan.report.destination.imageId : undefined,
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
