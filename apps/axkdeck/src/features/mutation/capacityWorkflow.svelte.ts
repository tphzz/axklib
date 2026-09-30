import { ImportCapacityReview } from '../import/importCapacityReview.svelte';
import type { ASeriesPreferences } from '../../lib/aSeriesPreferences.svelte';
import type { CapacityAdmission, CapacityPolicy } from '../../lib/importCapacity';
import { CapacityWriteRejected, type CapacityReviewContext } from '../../lib/httpCapacityGate';

interface CapacityMutationRequest {
    review: ImportCapacityReview;
    checking: boolean;
    check: (policy: CapacityPolicy) => Promise<CapacityAdmission>;
    resolve: (policy: CapacityPolicy | null) => void;
}

export class MutationCapacityWorkflow {
    request = $state<CapacityMutationRequest | null>(null);
    private pending = false;
    private disposed = false;

    constructor(private readonly preferences: ASeriesPreferences) {}

    async review(
        check: CapacityMutationRequest['check'],
        context: CapacityReviewContext = { emptyVolumeCreation: false },
    ): Promise<CapacityPolicy> {
        if (this.pending || this.disposed) throw new CapacityWriteRejected('Sampler capacity review is unavailable');
        this.pending = true;
        try {
            await this.preferences.ready;
            const review = new ImportCapacityReview();
            review.target = this.preferences.generation;
            const initial = await check(review.policy());
            if (this.disposed) throw new CapacityWriteRejected('Capacity review was closed; nothing was written');
            if (initial.target !== review.target) throw new CapacityWriteRejected('Capacity target does not match');
            const emptyVolumes =
                context.emptyVolumeCreation &&
                initial.reports.every((report) => report.objectCounts.every((objects) => objects.count === 0));
            if (initial.allowed && (!initial.reports.length || emptyVolumes)) return review.policy();
            review.admission = initial;
            const policy = await new Promise<CapacityPolicy | null>((resolve) => {
                this.request = { review, checking: false, check, resolve };
            });
            if (!policy) throw new CapacityWriteRejected('Sampler capacity review cancelled; nothing was written');
            return policy;
        } finally {
            this.pending = false;
        }
    }

    async inspect(): Promise<void> {
        const request = this.request;
        if (!request || request.checking || request.review.busy) return;
        request.checking = true;
        request.review.admission = null;
        request.review.message = '';
        const policy = request.review.policy();
        try {
            const admission = await request.check(policy);
            if (this.request !== request) return;
            if (admission.target !== policy.target) throw new Error('Capacity target does not match');
            request.review.admission = admission;
        } catch (error) {
            if (this.request === request)
                request.review.message = error instanceof Error ? error.message : 'Capacity inspection failed';
        } finally {
            request.checking = false;
        }
    }

    async submit(): Promise<void> {
        const request = this.request;
        if (!request || request.checking || request.review.busy) return;
        try {
            const policy = await request.review.review(request.check);
            if (this.request !== request || !policy) return;
            this.request = null;
            request.resolve(policy);
        } catch {
            // The review retains the refusal or technical error for correction.
        }
    }

    cancel(): void {
        const request = this.request;
        if (!request || request.review.busy) return;
        this.request = null;
        request.resolve(null);
    }

    dispose(): void {
        this.disposed = true;
        const request = this.request;
        this.request = null;
        request?.resolve(null);
    }
}
