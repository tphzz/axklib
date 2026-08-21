import type { ClientUploadLocation, InputFileLocation } from '../../lib/storageLocations';
import type {
    ImageSessionPackageImportPlan,
    ImageTransport,
    PackageInspection,
    PackageOpaqueSequenceDecision,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import type { ImportDestinationMode } from './packageDestinations';
import type { PackagePickerHistory } from './packagePickerHistory';

export type PackageBatchDestinationStrategy = 'shared' | 'separate';

export interface BatchPackageItem {
    id: string;
    selected: boolean;
    source: InputFileLocation;
    sourceName: string;
    inspection: PackageInspection;
    upload: ClientUploadLocation | null;
    localPath: string | null;
}

export interface PackageBatchImportRequest {
    item: DiskTreeItem | null;
    canChangeSources: boolean;
    items: BatchPackageItem[];
    plan: ImageSessionPackageImportPlan | null;
    destinationStrategy: PackageBatchDestinationStrategy;
    destinationMode: ImportDestinationMode;
    destinationPartitionIndex: number | null;
    destinationVolumeName: string;
    volumeNames: Record<string, string>;
    renames: Record<string, string>;
    programSlots: Record<string, number>;
    opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
    hasUnvalidatedChanges: boolean;
    status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
    completedFiles: number;
    totalFiles: number;
    progress: number;
    error: string;
}

export interface PackageBatchImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    pickerHistory: PackagePickerHistory;
    isDesktop: boolean;
    sessionId: () => number | null;
    invalidateSession: (sessionId: number) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
    mutationsAvailable?: () => boolean;
    sourceItems?: () => DiskTreeItem[];
}
