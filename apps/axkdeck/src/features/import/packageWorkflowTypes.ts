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

export interface PackageImportRequest {
    item: DiskTreeItem | null;
    canChangeSource: boolean;
    source: InputFileLocation | null;
    upload: ClientUploadLocation | null;
    localSourcePath: string | null;
    sourceName: string;
    destinationMode: ImportDestinationMode;
    destinationPartitionIndex: number | null;
    destinationVolumeName: string;
    inspection: PackageInspection | null;
    plan: ImageSessionPackageImportPlan | null;
    renames: Record<string, string>;
    programSlots: Record<string, number>;
    opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
    hasUnvalidatedChanges: boolean;
    status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
    progress: number;
    error: string;
}

export interface PackageImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    invalidateSession: (sessionId: number) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
    pickerHistory?: PackagePickerHistory;
    mutationsAvailable?: () => boolean;
    selectedSource?: () => DiskTreeItem;
    sourceItems?: () => DiskTreeItem[];
}
