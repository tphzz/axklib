import type { ASeriesPreferences } from '../../../lib/aSeriesPreferences.svelte';
import { createTransport } from '../../../lib/createTransport';
import { MutationCapacityWorkflow } from '../../mutation/capacityWorkflow.svelte';

export function createCapacityConnection(preferences: ASeriesPreferences) {
    const capacity = new MutationCapacityWorkflow(preferences);
    const transport = createTransport((check, context) => capacity.review(check, context));
    return { transport, capacity };
}
