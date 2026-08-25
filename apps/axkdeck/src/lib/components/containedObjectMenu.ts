import type { ObjectRenameTarget, PackageExportObject, SampleStructureItem, WaveDataItem } from '../types';

export type ContainedSelectionScope = 'sample-banks' | 'samples' | 'wave-data';
export type ContainedSelectableItem = SampleStructureItem | WaveDataItem;

export interface ContainedObjectMenuState {
    directWav: boolean;
    renameTarget: ObjectRenameTarget;
    objects: PackageExportObject[];
    sampleBankMembers: SampleStructureItem[] | null;
    sampleBankAssignmentMembers: SampleStructureItem[] | null;
    left: number;
    top: number;
}
