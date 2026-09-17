import type { AuditionWorkflow } from '../../audition/workflow.svelte';
import type { CatalogWorkflow } from '../../catalog/workflow.svelte';
import type { AudioImportWorkflow } from '../../import/audioWorkflow.svelte';
import type { SequenceImportWorkflow } from '../../import/sequenceWorkflow.svelte';
import type { MutationWorkflow } from '../../mutation/workflow.svelte';
import type { InterfaceScaleController } from '../../../lib/interfaceScale';
import type { ImageLocation } from '../../../lib/storageLocations';
import type { ImageTransport } from '../../../lib/transport';
import type {
    DiskTreeItem,
    ImageTreeAction,
    InspectorSelection,
    PackageExportObject,
    PackageExportSelection,
    Program,
    SampleStructureItem,
    SequenceItem,
    WaveDataItem,
    WorkspaceView,
} from '../../../lib/types';
import type { ObjectSelectionMode, PackageExportSelectionState } from '../../../lib/objectSelection';
import type { WorkspaceMode } from '../../workspace/contracts';
import type { FilesystemMutationDriver } from '../../../lib/filesystem';
import type { FilesystemExportActions } from '../../../lib/filesystemExport';
import type { AxklibFilesystemImports } from './su700Actions';

interface WorkspaceTab {
    id: WorkspaceView;
    label: string;
    icon: 'music' | 'layers' | 'archive' | 'waveform' | 'list';
}

export interface WorkspaceProps {
    mode?: WorkspaceMode;
    revision?: number;
    transport: ImageTransport;
    filesMutations?: (sessionId: number) => FilesystemMutationDriver;
    filesExports?: (sessionId: number) => FilesystemExportActions;
    filesImports?: (sessionId: number) => AxklibFilesystemImports;
    interfaceScaling?: InterfaceScaleController | null;
    isDesktop: boolean;
    workspaceTabs: WorkspaceTab[];
    workspaceView: WorkspaceView;
    inspectorOpen?: boolean;
    imageLocation: ImageLocation | null;
    sourceItems: DiskTreeItem[];
    selectedSource: DiskTreeItem;
    selectedVolumeIds: readonly string[];
    imageOpening: boolean;
    sessionId: number | null;
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    mutation: MutationWorkflow;
    audioImport: AudioImportWorkflow;
    sequenceImport: SequenceImportWorkflow;
    importAudio: () => void;
    importMidi: () => void;
    programs: Program[];
    sampleBanks: SampleStructureItem[];
    samples: SampleStructureItem[];
    waveData: WaveDataItem[];
    sequences: SequenceItem[];
    bankMembers: SampleStructureItem[];
    bankMemberWaveData: WaveDataItem[];
    sampleWaveData: WaveDataItem[];
    activeCollectionObjectId: string;
    inspectorSelection: InspectorSelection;
    editorSelection: InspectorSelection;
    sourceStatus: string;
    packageSelection: PackageExportSelectionState;
    objectDeletionAvailable: boolean;
    waveDataCleanupAvailable: boolean;
    programGenerationAvailable: boolean;
    programAssignmentCleanupAvailable: boolean;
    packageImportAvailable: boolean;
    packageExportAvailable: boolean;
    volumePackageExportAvailable: boolean;
    volumeFloppyExportAvailable: boolean;
    audioExportAvailable: boolean;
    sequenceExportAvailable: boolean;
    mediaConversionAvailable: boolean;
    allocationInspectionAvailable: boolean;
    samplerOrderingEnabled?: boolean;
    openConnectionSettings: () => void;
    openImage: () => void;
    createImage: () => void;
    closeImage: () => void;
    showImageIntegrity: () => void;
    manageLocations: () => void;
    selectSource: (item: DiskTreeItem, mode: ObjectSelectionMode, visibleVolumes: readonly DiskTreeItem[]) => void;
    selectSourceForContext: (item: DiskTreeItem, visibleVolumes: readonly DiskTreeItem[]) => void;
    imageAction: (item: DiskTreeItem, action: ImageTreeAction) => void;
    selectWorkspace: (view: WorkspaceView) => void;
    exportPackage: (items: PackageExportObject[]) => void;
    exportAudio: (items: PackageExportSelection[]) => void;
    exportWav: (items: PackageExportSelection[]) => void;
    exportMidi: (items: PackageExportObject[]) => void;
    deleteObjects: (items: PackageExportObject[]) => void;
    cleanupWaveData: () => void;
    generatePrograms: () => void;
    cleanupProgramAssignments: () => void;
    clearSelection: () => void;
    selectionChanged: (selection: PackageExportSelectionState) => void;
    selectionLimit: () => void;
    setStatus: (status: string) => void;
}
