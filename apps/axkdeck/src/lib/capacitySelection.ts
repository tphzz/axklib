import type { AuditionWorkflow } from '../features/audition/workflow.svelte';
import type { SampleStructureItem, WaveDataItem } from './types';
import type { VolumeCapacityController } from './volumeCapacity.svelte';

export function capacitySelection(capacity: VolumeCapacityController, audition: () => AuditionWorkflow) {
    return {
        bank(item: SampleStructureItem): void {
            capacity.showObject();
            void audition().selectBank(item);
        },
        member(item: SampleStructureItem): void {
            capacity.showObject();
            void audition().selectBankMember(item);
        },
        sample(item: SampleStructureItem): void {
            capacity.showObject();
            void audition().selectSample(item);
        },
        wave(item: WaveDataItem): void {
            capacity.showObject();
            void audition().selectWaveData(item);
        },
    };
}
