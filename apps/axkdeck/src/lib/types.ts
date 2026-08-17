import type { PreviewEnvelope, SamplerObject, SamplerRelationship } from './transport';

export type WorkspaceView = 'programs' | 'sequences' | 'sample-banks' | 'samples' | 'wave-data';
export type PackageExportObjectKind = 'PROGRAM' | 'SEQU' | 'SBAC' | 'SBNK' | 'SMPL';

export interface PackageExportObject {
    kind: PackageExportObjectKind;
    objectId: string;
    name: string;
    typeLabel: 'Program' | 'Sequence' | 'Sample Bank' | 'Sample' | 'Wave Data';
    partitionIndex: number;
    partitionName: string;
    volumeName: string;
}

export interface PackageExportVolume {
    kind: 'VOLUME';
    contentId: string;
    partitionIndex: number;
    volumeName: string;
    name: string;
    typeLabel: 'Volume';
}

export type PackageExportSelection = PackageExportObject | PackageExportVolume;

export interface DiskTreeItem {
    id: string;
    name: string;
    kind: 'disk' | 'partition' | 'volume' | 'category' | 'object';
    children?: DiskTreeItem[];
    childCount: number;
    objectId?: string;
    objectType?: string;
    scopeRole?: 'CONTAINED' | 'REFERENCE';
    volumeId?: string;
    volumeDirectoryId?: number;
    volumeName?: string;
    partitionIndex?: number;
}

export type ImageTreeAction =
    | 'add-volume'
    | 'rename-volume'
    | 'delete-volume'
    | 'rename-partition'
    | 'inspect-allocation'
    | 'repair-placement'
    | 'import-package'
    | 'import-packages'
    | 'export-package'
    | 'export-volume-packages'
    | 'export-volume-floppies'
    | 'export-sfz'
    | 'export-cdrom'
    | 'export-floppy';

export interface Program {
    id: string;
    objectId: string;
    slot: string;
    programNumber: number;
    name: string;
    object: SamplerObject;
}

export interface SequenceItem {
    id: string;
    objectId: string;
    name: string;
    object: SamplerObject;
}

export type ObjectRenameTarget =
    | { kind: 'program'; object: SamplerObject; name: string; programNumber: number }
    | { kind: 'sequence' | 'sample-bank' | 'sample' | 'wave-data'; object: SamplerObject; name: string };

export interface SampleStructureItem {
    id: string;
    objectId: string;
    name: string;
    objectType: 'SBAC' | 'SBNK';
    object: SamplerObject;
    sampleBankObjectIds?: string[];
    membershipLabel?: string;
    memberCount?: number;
}

export interface SampleBankAssignmentOption {
    objectId: string;
    name: string;
    memberCount: number;
    selectedMemberCount: number;
    movedSampleCount: number;
    reassignedSampleCount: number;
    finalMemberCount: number;
}

export interface SampleBankAssignmentBlocker {
    sampleName: string;
    programName: string;
}

export interface WaveDataItem {
    id: string;
    name: string;
    note: string;
    duration: string;
    sampleRate: string;
    bitDepth: string;
    channels: 'Mono' | 'Stereo';
    storedSizeBytes: number;
    waveform: readonly WaveformBin[];
    previewState: 'idle' | 'loading' | 'ready' | 'failed';
    objectKey: string;
    object: SamplerObject;
}

export interface LinkedWaveDataItem {
    role: 'left' | 'right';
    waveData: WaveDataItem;
}

export interface SampleWaveformPreview {
    item: SampleStructureItem;
    waveData: LinkedWaveDataItem[];
    preview: PreviewEnvelope | null;
    previewState: 'idle' | 'loading' | 'ready' | 'failed';
}

export interface WaveformBin {
    minimum: number;
    maximum: number;
}

export interface ProgramAssignmentRow {
    relationship: SamplerRelationship;
    targetType: string;
    targetName: string;
    targetObjectId?: string;
    confirmed: boolean;
}

export interface ProgramSampleSelectRow {
    id: string;
    targetType: string;
    targetName: string;
    targetObjectId?: string;
    navigable: boolean;
    assigned: boolean;
    receiveChannelDisplays: string[];
    sourceLoad: boolean;
    relationships: SamplerRelationship[];
}

export interface ProgramSampleSelectRows {
    assigned: ProgramSampleSelectRow[];
    all: ProgramSampleSelectRow[];
}

export type InspectorSelection =
    | {
          kind: 'program';
          program: Program;
          assignments: ProgramAssignmentRow[];
          sampleSelect: ProgramSampleSelectRows;
      }
    | { kind: 'sequence'; sequence: SequenceItem }
    | {
          kind: 'sample-bank';
          item: SampleStructureItem;
          members: SampleStructureItem[];
          memberPreviews: SampleWaveformPreview[];
          displayedMemberId: string;
      }
    | {
          kind: 'sample';
          item: SampleStructureItem;
          memberships: SampleStructureItem[];
          preview: SampleWaveformPreview;
      }
    | { kind: 'wave-data'; waveData: WaveDataItem }
    | null;
