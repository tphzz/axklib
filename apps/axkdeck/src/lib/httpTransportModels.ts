import type { components } from './generated/axklibApiV1';
import type { ImageLocation } from './storageLocations';
import type {
    PlanSummary,
    RelationshipQuality,
    SamplerObject,
    SamplerRelationship,
    ValidationSummary,
} from './transport';
import type { DiskTreeItem } from './types';

export interface ApiImageSummary {
    imageId: string;
    revision: number;
    source: components['schemas']['ImageSourceRef'];
    companionSources: components['schemas']['ImageSourceRef'][];
    floppySet: components['schemas']['ImageFloppySet'] | null;
    format: string;
    rootCount: number;
    objectCount: number;
    relationshipCount: number;
    availableOperations?: string[];
    validation: { valid: boolean; infoCount: number; warningCount: number; errorCount: number };
}

export type ApiContentItem = components['schemas']['ImageContentItem'];
export type ApiObjectItem = components['schemas']['ImageObjectItem'];
export type ApiRelationshipItem = components['schemas']['ImageRelationshipItem'];
export type ApiObjectDeletionInspection = components['schemas']['ImageObjectDeletionInspection'];
export type ApiWaveDataOrphanInspection = components['schemas']['ImageWaveDataOrphanInspection'];

export interface ApiPage<Item> {
    items: Item[];
    totalCount: number;
    nextCursor: string | null;
}

export interface ApiWritePlan {
    planToken: string;
    kind: string;
    summary: Record<string, unknown>;
}

export interface ApiAlterationInspection {
    kind: string;
    summary: Record<string, unknown>;
}

export interface SessionState {
    remoteId: string;
    revision: number;
    source: ImageLocation;
    contentCursors: Map<string, Map<number, string | null>>;
    contentItems: Map<string, DiskTreeItem>;
    objectCursors: Map<string, Map<number, string | null>>;
    relationshipCursors: Map<string, Map<number, string | null>>;
}

function itemKind(kind: string): DiskTreeItem['kind'] {
    if (kind === 'partition') return 'partition';
    if (kind === 'volume') return 'volume';
    if (kind === 'category') return 'category';
    return 'object';
}

export function mapContentItem(item: ApiContentItem, parent?: DiskTreeItem): DiskTreeItem {
    const kind = itemKind(item.kind);
    const name = item.name ?? item.displayName;
    const volumeId = kind === 'volume' ? item.id : parent?.volumeId;
    const volumeName = kind === 'volume' ? name : parent?.volumeName;
    const partitionIndex = item.partitionIndex ?? parent?.partitionIndex;
    return {
        id: item.id,
        name,
        kind,
        childCount: item.childCount,
        objectId: item.objectId ?? undefined,
        objectType: item.objectType ?? undefined,
        scopeRole: item.scopeRole,
        volumeId,
        volumeDirectoryId: item.volumeDirectoryId ?? undefined,
        volumeName,
        partitionIndex,
        partitionCapacity: item.partitionCapacity ?? undefined,
        sizeBytes: item.sizeBytes ?? undefined,
    };
}

export function mapObject(item: ApiObjectItem): SamplerObject {
    return {
        key: item.id,
        objectType: item.type,
        name: item.name,
        partitionIndex: item.partitionIndex ?? 0,
        partitionName: item.partitionName,
        volumeName: item.volumeName,
        categoryName: item.categoryName,
        objectEncoding: item.format,
        directoryEntryName: item.entryName,
        sfsId: 0,
        storedSizeBytes: item.sizeBytes,
        sizeWithDependenciesBytes: item.sizeWithDependenciesBytes,
        sampleRate: item.waveform?.sampleRate ?? 0,
        rootKey: item.waveform?.rootKey ?? 0,
        frameCount: item.waveform?.frameCount ?? 0,
        sampleWidthBytes: item.waveform?.sampleWidthBytes ?? 0,
        sourceWaveName: item.waveform?.sourceWaveName,
        fineTuneCents: item.waveform?.fineTuneCents,
        loopMode: item.waveform?.loopMode,
        loopModeLabel: item.waveform?.loopModeLabel,
        loopStartFrame: item.waveform?.loopStartFrame,
        loopLengthFrames: item.waveform?.loopLengthFrames,
        sequence: item.sequence
            ? {
                  formatVersion: item.sequence.formatVersion,
                  ticksPerQuarterNote: item.sequence.ticksPerQuarterNote,
                  firstTick: item.sequence.firstTick,
                  endTick: item.sequence.endTick,
                  eventCount: item.sequence.eventCount,
                  headerTempoBpm: item.sequence.headerTempoBpm ?? undefined,
                  effectiveInitialTempoMicrosecondsPerQuarterNote:
                      item.sequence.effectiveInitialTempoMicrosecondsPerQuarterNote,
                  tempoEvents: item.sequence.tempoEvents,
              }
            : undefined,
    };
}

export function mapRelationship(item: ApiRelationshipItem): SamplerRelationship {
    return {
        id: item.id,
        sourceObjectId: item.sourceObjectId,
        targetObjectId: item.targetObjectId ?? undefined,
        candidateObjectIds: item.candidateObjectIds,
        relationshipType: item.type,
        quality: relationshipQuality(item.quality),
        basis: item.basis,
        notes: item.notes,
        assignmentIndex: item.assignmentIndex ?? undefined,
        assignmentName: item.assignmentName,
        assignmentState: item.assignmentState,
        receiveChannelDisplay: item.receiveChannelDisplay,
    };
}

function relationshipQuality(value: string): RelationshipQuality {
    switch (value) {
        case 'KNOWN':
        case 'LIKELY':
        case 'TENTATIVE':
        case 'UNKNOWN':
            return value;
        default:
            throw new Error(`Unsupported relationship quality: ${value}`);
    }
}

export function validationSummary(summary: ApiImageSummary): ValidationSummary {
    const validation = summary.validation;
    return {
        valid: validation.valid,
        issueCount: validation.infoCount + validation.warningCount + validation.errorCount,
        errorCount: validation.errorCount,
        warningCount: validation.warningCount,
        objectCount: summary.objectCount,
        relationshipCount: summary.relationshipCount,
    };
}

export function planSummary(plan: ApiWritePlan): PlanSummary {
    return {
        partitionCount: Number(plan.summary.partitionCount ?? 0),
        operationCount: Number(plan.summary.operationCount ?? 0),
        sizeBytes: Number(plan.summary.sizeBytes ?? 0),
        appliesChanges: true,
        planToken: plan.planToken,
    };
}
