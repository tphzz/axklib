import type { SamplerObject, SamplerRelationship } from './transport';

export type WorkspaceView = 'programs' | 'sample-banks' | 'samples' | 'wave-data';

export interface DiskTreeItem {
    id: string;
    name: string;
    kind: 'disk' | 'partition' | 'volume' | 'category' | 'object';
    children?: DiskTreeItem[];
    childCount: number;
    objectId?: string;
    objectType?: string;
    volumeId?: string;
    volumeName?: string;
    partitionIndex?: number;
}

export type ImageTreeAction = 'add-volume' | 'rename-volume' | 'delete-volume' | 'rename-partition';

export interface Program {
    id: string;
    objectId: string;
    slot: string;
    name: string;
    object: SamplerObject;
}

export interface SampleStructureItem {
    id: string;
    objectId: string;
    name: string;
    objectType: 'SBAC' | 'SBNK';
    object: SamplerObject;
    membershipLabel?: string;
    memberCount?: number;
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

export interface WaveformBin {
    minimum: number;
    maximum: number;
}

export interface ProgramAssignmentRow {
    relationship: SamplerRelationship;
    targetType: string;
    targetName: string;
    targetObjectId?: string;
}

export type InspectorSelection =
    | { kind: 'program'; program: Program; assignments: ProgramAssignmentRow[] }
    | { kind: 'sample-bank'; item: SampleStructureItem; members: SampleStructureItem[] }
    | {
          kind: 'sample';
          item: SampleStructureItem;
          memberships: SampleStructureItem[];
          waveData: LinkedWaveDataItem[];
      }
    | { kind: 'wave-data'; waveData: WaveDataItem }
    | null;
