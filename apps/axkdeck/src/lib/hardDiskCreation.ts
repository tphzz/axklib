import type { components } from './generated/axklibApiV1';
export type HardDiskCreationProfileId = components['schemas']['HardDiskCreationProfile']['profileId'];

export interface HardDiskCreationPartitionOption {
    partitionCount: number;
    partitionSizeBytes: number;
    unusedTailBytes: number;
}

export interface HardDiskCreationProfile {
    profileId: HardDiskCreationProfileId;
    sizeBytes: number;
    defaultPartitionCount: number;
    partitionOptions: HardDiskCreationPartitionOption[];
}
