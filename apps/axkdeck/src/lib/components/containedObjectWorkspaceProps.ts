import type { PackageExportSelectionState } from '../objectSelection';
import type { ObjectRenameTarget, PackageExportObject, SampleStructureItem, WaveDataItem } from '../types';

export type ContainedLaneId = 'primary' | 'secondary' | 'tertiary';

export interface ContainedObjectWorkspaceProps {
    view: 'sample-banks' | 'samples';
    sampleBanks: SampleStructureItem[];
    samples: SampleStructureItem[];
    waveData: WaveDataItem[];
    activeSampleBankId: string;
    activeSampleId: string;
    activeWaveDataId: string;
    queries: Record<ContainedLaneId, string>;
    showOnlyStandaloneSamples?: boolean;
    onshowonlystandalonechange?: (checked: boolean) => void;
    onquerychange: (lane: ContainedLaneId, value: string) => void;
    onsamplebankselect: (item: SampleStructureItem) => void;
    onsampleselect: (item: SampleStructureItem) => void;
    onwavedataselect: (item: WaveDataItem) => void;
    onplaysamplebank?: (item: SampleStructureItem) => void;
    onplaysample?: (item: SampleStructureItem) => void;
    onplaywavedata?: (item: WaveDataItem) => void;
    onstop?: () => void;
    onimportaudio?: () => void;
    playingSampleBankId?: string;
    playingObjectId?: string | null;
    preparingObjectId?: string | null;
    auditionableSampleIds: ReadonlySet<string>;
    auditionableSampleBankIds: ReadonlySet<string>;
    stereoSampleIds?: ReadonlySet<string>;
    objectRenameAvailable?: boolean;
    onrenameobject?: (target: ObjectRenameTarget) => void;
    sampleBankCreationAvailable?: boolean;
    oncreatesamplebank?: (samples: SampleStructureItem[]) => void;
    sampleBankAssignmentAvailable?: boolean;
    onassignsamplebank?: (samples: SampleStructureItem[]) => void;
    objectDeletionAvailable?: boolean;
    ondeleteobjects?: (objects: PackageExportObject[]) => void;
    packageExportAvailable?: boolean;
    onexportobjects?: (objects: PackageExportObject[]) => void;
    audioExportAvailable?: boolean;
    onexportaudio?: (objects: PackageExportObject[]) => void;
    onexportwav?: (objects: PackageExportObject[]) => void;
    selection?: PackageExportSelectionState;
    onselectionchange?: (selection: PackageExportSelectionState) => void;
    onselectionlimit?: () => void;
}
