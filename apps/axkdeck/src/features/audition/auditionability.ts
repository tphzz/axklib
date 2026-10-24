import { auditionableSampleBankIds, auditionableSampleIds, stereoSampleIds } from '../../lib/sampleRelationships';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';

export function auditionabilityIndex(catalog: CatalogWorkflow, cached?: AuditionabilityIndex) {
    if (
        cached?.relationships === catalog.relationships &&
        cached.waveData === catalog.waveData &&
        cached.sampleBanks === catalog.sampleBanks &&
        cached.samples === catalog.samples
    )
        return cached;
    const sampleIds = auditionableSampleIds(catalog.relationships, catalog.waveData);
    return {
        relationships: catalog.relationships,
        waveData: catalog.waveData,
        sampleBanks: catalog.sampleBanks,
        samples: catalog.samples,
        sampleIds,
        sampleBankIds: auditionableSampleBankIds(
            catalog.relationships,
            catalog.sampleBanks,
            catalog.samples,
            sampleIds,
        ),
        stereoIds: stereoSampleIds(catalog.relationships, catalog.waveData),
        objectIds: new Set([...sampleIds, ...catalog.waveData.map((item) => item.objectKey)]),
    };
}

export interface AuditionabilityIndex {
    relationships: CatalogWorkflow['relationships'];
    waveData: CatalogWorkflow['waveData'];
    sampleBanks: CatalogWorkflow['sampleBanks'];
    samples: CatalogWorkflow['samples'];
    objectIds: Set<string>;
    sampleIds: Set<string>;
    sampleBankIds: Set<string>;
    stereoIds: Set<string>;
}
