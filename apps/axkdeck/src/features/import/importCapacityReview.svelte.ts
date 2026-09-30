import type { CapacityAdmission, CapacityPolicy } from '../../lib/importCapacity';
import { capacityFailure } from '../../lib/importCapacity';
import type { ASeriesGeneration } from '../../lib/aSeriesPreferences.svelte';

export class ImportCapacityReview {
    target = $state<ASeriesGeneration>('A3000');
    admission = $state<CapacityAdmission | null>(null);
    busy = $state(false);
    message = $state('');

    policy(): CapacityPolicy {
        return { target: this.target };
    }

    async review(check: (policy: CapacityPolicy) => Promise<CapacityAdmission>): Promise<CapacityPolicy | null> {
        if (this.busy) return null;
        this.busy = true;
        this.message = 'Checking sampler load capacity';
        this.admission = null;
        const policy = this.policy();
        try {
            const admission = await check(policy);
            if (admission.target !== policy.target || this.target !== policy.target)
                throw new Error('Sampler load target changed; review again.');
            this.admission = admission;
            if (admission.allowed) {
                this.message = 'Sampler load capacity checked';
                return policy;
            }
            this.message = capacityFailure(admission);
            throw new Error(this.message);
        } catch (error) {
            this.message = error instanceof Error ? error.message : 'Sampler load capacity check failed';
            throw error;
        } finally {
            this.busy = false;
        }
    }

    reset(): void {
        if (this.busy) return;
        this.admission = null;
        this.message = '';
    }
}
