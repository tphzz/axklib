export type HardDiskCreationProfileId = 'FLOPPY_SCALE' | 'CD_R_650' | 'CD_R_700' | 'HDS_1_GIB' | 'HDS_2_GIB';

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
