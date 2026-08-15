import { objectPresentationName } from '../../lib/objectPresentation';
import { collectPages } from '../../lib/pagination';
import { programSampleSelectRows } from '../../lib/programSampleSelect';
import { isConfirmedRelationship } from '../../lib/relationshipResolution';
import {
    distinctWaveDataForSample,
    linkedWaveDataForSample,
    orderedSamplesForBank,
} from '../../lib/sampleRelationships';
import type { ImageTransport, SamplerObject, SamplerRelationship, SystemProgramContexts } from '../../lib/transport';
import type {
    DiskTreeItem,
    InspectorSelection,
    Program,
    ProgramAssignmentRow,
    SampleStructureItem,
    SampleWaveformPreview,
    SequenceItem,
    WaveDataItem,
    WorkspaceView,
} from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';

interface CatalogWorkflowDependencies {
    transport: ImageTransport;
    sessionId: () => number | null;
    stopPlayback: () => Promise<void>;
    resetPreviews: () => void;
    resetCleanup: () => void;
    setStatus: (status: string) => void;
}

export class CatalogWorkflow {
    programs = $state<Program[]>([]);
    sequences = $state<SequenceItem[]>([]);
    sampleBanks = $state<SampleStructureItem[]>([]);
    samples = $state<SampleStructureItem[]>([]);
    waveData = $state<WaveDataItem[]>([]);
    relationships = $state<SamplerRelationship[]>([]);
    objectsById = $state(new Map<string, SamplerObject>());
    selectedProgramId = $state('');
    selectedSequenceId = $state('');
    selectedBankId = $state('');
    selectedBankMemberId = $state('');
    selectedSampleId = $state('');
    selectedBankWaveDataId = $state('');
    selectedSampleWaveDataId = $state('');
    selectedWaveDataId = $state('');
    inspectorObjectId = $state('');
    editorObjectIds = $state<Record<WorkspaceView, string>>({
        programs: '',
        sequences: '',
        'sample-banks': '',
        samples: '',
        'wave-data': '',
    });
    samplePreviewStates = $state<Record<string, Pick<SampleWaveformPreview, 'preview' | 'previewState'>>>({});
    activeVolumeId = $state('');
    activePartitionIndex = $state<number | null>(null);
    systemProgramContexts = $state<SystemProgramContexts | null>(null);
    systemProgramContextsLoading = $state(false);
    systemProgramContextsError = $state('');
    objectCount = $state(0);
    private loadGeneration = 0;

    constructor(private readonly dependencies: CatalogWorkflowDependencies) {}

    membersForBank(bankId: string): SampleStructureItem[] {
        return orderedSamplesForBank(bankId, this.relationships, this.samples);
    }

    banksForSample(sampleId: string): SampleStructureItem[] {
        const ids = new Set(
            this.relationships
                .filter(
                    (item) =>
                        isConfirmedRelationship(item) &&
                        item.targetObjectId === sampleId &&
                        item.relationshipType === 'SBAC_SLOT_TO_SBNK',
                )
                .map((item) => item.sourceObjectId),
        );
        return [...ids]
            .map((id) => this.sampleBanks.find((item) => item.objectId === id))
            .filter((item) => item !== undefined);
    }

    waveDataForSample(sampleId: string): WaveDataItem[] {
        return distinctWaveDataForSample(sampleId, this.relationships, this.waveData);
    }

    assignmentsForProgram(programId: string): ProgramAssignmentRow[] {
        return this.relationships
            .filter((item) => item.sourceObjectId === programId && item.relationshipType.startsWith('PROG_ASSIGNMENT_'))
            .toSorted((left, right) => (left.assignmentIndex ?? 0) - (right.assignmentIndex ?? 0))
            .map((relationship) => {
                const target = relationship.targetObjectId
                    ? this.objectsById.get(relationship.targetObjectId)
                    : undefined;
                const targetName = relationship.targetObjectId
                    ? (this.sampleBanks.find((item) => item.objectId === relationship.targetObjectId)?.name ??
                      this.samples.find((item) => item.objectId === relationship.targetObjectId)?.name ??
                      this.waveData.find((item) => item.objectKey === relationship.targetObjectId)?.name)
                    : undefined;
                return {
                    relationship,
                    targetObjectId: relationship.targetObjectId,
                    targetType: target?.objectType ?? relationship.relationshipType.replace('PROG_ASSIGNMENT_TO_', ''),
                    targetName: targetName || target?.name || relationship.assignmentName || 'Unresolved assignment',
                    confirmed: isConfirmedRelationship(relationship),
                };
            });
    }

    sampleWaveformPreview(item: SampleStructureItem): SampleWaveformPreview {
        const stored = this.samplePreviewStates[item.objectId];
        return {
            item,
            waveData: linkedWaveDataForSample(item.objectId, this.relationships, this.waveData),
            preview: stored?.preview ?? null,
            previewState: stored?.previewState ?? 'idle',
        };
    }

    selectionForObject(objectId: string, displayedBankMemberId = ''): InspectorSelection {
        const program = this.programs.find((item) => item.objectId === objectId);
        if (program) {
            const assignments = this.assignmentsForProgram(program.objectId);
            return {
                kind: 'program',
                program,
                assignments,
                sampleSelect: programSampleSelectRows(assignments, this.sampleBanks, this.samples),
            };
        }
        const bank = this.sampleBanks.find((item) => item.objectId === objectId);
        if (bank) {
            const members = this.membersForBank(bank.objectId);
            const displayedMemberId = members.some((member) => member.objectId === displayedBankMemberId)
                ? displayedBankMemberId
                : (members[0]?.objectId ?? '');
            return {
                kind: 'sample-bank',
                item: bank,
                members,
                memberPreviews: members.map((member) => this.sampleWaveformPreview(member)),
                displayedMemberId,
            };
        }
        const sample = this.samples.find((item) => item.objectId === objectId);
        if (sample) {
            return {
                kind: 'sample',
                item: sample,
                memberships: this.banksForSample(sample.objectId),
                preview: this.sampleWaveformPreview(sample),
            };
        }
        const sequence = this.sequences.find((item) => item.objectId === objectId);
        if (sequence) return { kind: 'sequence', sequence };
        const waveform = this.waveData.find((item) => item.objectKey === objectId);
        return waveform ? { kind: 'wave-data', waveData: waveform } : null;
    }

    async loadVolume(volumeId: string, partitionIndex: number | null = this.activePartitionIndex): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null) return;
        this.dependencies.resetCleanup();
        void this.dependencies.stopPlayback();
        this.dependencies.resetPreviews();
        this.activeVolumeId = volumeId;
        this.activePartitionIndex = partitionIndex;
        const generation = ++this.loadGeneration;
        this.loadSystemProgramContexts(sessionId, partitionIndex, generation);
        this.dependencies.setStatus('Loading volume');
        this.inspectorObjectId = '';
        try {
            const [objects, scopedRelationships, names] = await Promise.all([
                this.allObjects(sessionId, volumeId),
                this.allRelationships(sessionId, volumeId),
                this.visibleObjectNames(sessionId, volumeId),
            ]);
            if (generation !== this.loadGeneration) return;
            this.relationships = scopedRelationships;
            this.objectsById = new Map(objects.map((object) => [object.key, object]));
            this.programs = programItems(objects, names);
            this.sequences = sequenceItems(objects, names);
            const bankObjects = objects.filter((object) => object.objectType === 'SBAC');
            this.sampleBanks = bankItems(bankObjects, scopedRelationships, names);
            this.samples = sampleItems(objects, bankObjects, scopedRelationships, names);
            this.setWaveDataObjects(objects, names);
            this.objectCount = objects.length;
            this.clearSelections();
            this.dependencies.setStatus('Ready');
        } catch (error) {
            if (generation === this.loadGeneration) {
                this.activeVolumeId = '';
                this.dependencies.setStatus(userFacingMessage(error));
            }
        }
    }

    clear(): void {
        this.dependencies.resetCleanup();
        void this.dependencies.stopPlayback();
        this.dependencies.resetPreviews();
        ++this.loadGeneration;
        this.programs = [];
        this.sequences = [];
        this.sampleBanks = [];
        this.samples = [];
        this.waveData = [];
        this.relationships = [];
        this.objectsById = new Map();
        this.clearSelections();
        this.objectCount = 0;
        this.activeVolumeId = '';
        this.activePartitionIndex = null;
        this.systemProgramContexts = null;
        this.systemProgramContextsLoading = false;
        this.systemProgramContextsError = '';
    }

    private clearSelections(): void {
        this.inspectorObjectId = '';
        this.selectedProgramId = '';
        this.selectedSequenceId = '';
        this.selectedBankId = '';
        this.selectedBankMemberId = '';
        this.selectedSampleId = '';
        this.selectedBankWaveDataId = '';
        this.selectedSampleWaveDataId = '';
        this.selectedWaveDataId = '';
        this.editorObjectIds = { programs: '', sequences: '', 'sample-banks': '', samples: '', 'wave-data': '' };
    }

    clearSampleSelection(): void {
        const hiddenIds = new Set([this.selectedSampleId, this.selectedSampleWaveDataId, this.editorObjectIds.samples]);
        this.selectedSampleId = '';
        this.selectedSampleWaveDataId = '';
        this.editorObjectIds = { ...this.editorObjectIds, samples: '' };
        if (hiddenIds.has(this.inspectorObjectId)) this.inspectorObjectId = '';
    }

    private loadSystemProgramContexts(sessionId: number, partitionIndex: number | null, generation: number): void {
        this.systemProgramContexts = null;
        this.systemProgramContextsError = '';
        this.systemProgramContextsLoading = partitionIndex !== null;
        if (partitionIndex === null) return;
        void Promise.resolve()
            .then(() => this.dependencies.transport.systemProgramContexts(sessionId, partitionIndex))
            .then(
                (contexts) => {
                    if (generation !== this.loadGeneration) return;
                    this.systemProgramContexts = contexts;
                    this.systemProgramContextsLoading = false;
                },
                () => {
                    if (generation !== this.loadGeneration) return;
                    this.systemProgramContextsError = "Could not read the partition's saved System Files.";
                    this.systemProgramContextsLoading = false;
                },
            );
    }

    private setWaveDataObjects(objects: SamplerObject[], names: Map<string, string>): void {
        const previews = new Map(
            this.waveData.map(
                (item) => [item.id, { waveform: item.waveform, previewState: item.previewState }] as const,
            ),
        );
        this.waveData = objects
            .filter((object) => object.objectType === 'SMPL')
            .map((object) => {
                const preview = previews.get(object.key);
                return {
                    id: object.key,
                    objectKey: object.key,
                    object,
                    name: objectPresentationName(object, names),
                    note: noteName(object.rootKey),
                    duration:
                        object.sampleRate > 0 ? `${(object.frameCount / object.sampleRate).toFixed(2)} s` : 'Unknown',
                    sampleRate: object.sampleRate > 0 ? `${(object.sampleRate / 1000).toFixed(1)} kHz` : 'Unknown',
                    bitDepth: object.sampleWidthBytes > 0 ? `${object.sampleWidthBytes * 8}-bit` : 'Unknown',
                    channels: 'Mono' as const,
                    storedSizeBytes: object.storedSizeBytes,
                    waveform: preview?.waveform ?? [],
                    previewState: preview?.previewState ?? 'idle',
                };
            });
    }

    private async allContentChildren(sessionId: number, parentId: string): Promise<DiskTreeItem[]> {
        return collectPages(
            (offset, limit) => this.dependencies.transport.contentChildren(sessionId, parentId, offset, limit),
            { key: (item) => item.id, cancelled: () => this.dependencies.sessionId() !== sessionId },
        );
    }

    private async visibleObjectNames(sessionId: number, volumeId: string): Promise<Map<string, string>> {
        const volumeChildren = await this.allContentChildren(sessionId, volumeId);
        const categoryChildren = (
            await Promise.all(
                volumeChildren
                    .filter((item) => item.kind === 'category' && item.childCount > 0)
                    .map((item) => this.allContentChildren(sessionId, item.id)),
            )
        ).flat();
        return new Map(
            [...volumeChildren, ...categoryChildren]
                .filter((item) => item.objectId)
                .map((item) => [item.objectId!, item.name]),
        );
    }

    private async allObjects(sessionId: number, volumeId: string): Promise<SamplerObject[]> {
        return collectPages(
            async (offset, limit) => {
                const page = await this.dependencies.transport.objectPage(sessionId, offset, limit, {
                    scopeId: volumeId,
                });
                return { items: page.objects, totalCount: page.totalCount };
            },
            { key: (item) => item.key, cancelled: () => this.dependencies.sessionId() !== sessionId },
        );
    }

    private async allRelationships(sessionId: number, volumeId: string): Promise<SamplerRelationship[]> {
        return collectPages(
            async (offset, limit) => {
                const page = await this.dependencies.transport.relationshipPage(sessionId, offset, limit, {
                    scopeId: volumeId,
                });
                return { items: page.relationships, totalCount: page.totalCount };
            },
            { key: (item) => item.id, cancelled: () => this.dependencies.sessionId() !== sessionId },
        );
    }
}

function programItems(objects: SamplerObject[], names: Map<string, string>): Program[] {
    return objects
        .filter((object) => object.objectType === 'PROG')
        .map((object) => {
            const name = objectPresentationName(object, names);
            const match = /^(\d{3})(?::\s*)?(.*)$/.exec(name);
            return {
                id: object.key,
                objectId: object.key,
                object,
                slot: match?.[1] ?? object.name,
                programNumber: Number(match?.[1] ?? object.name),
                name: match?.[2] || name,
            };
        });
}

function sequenceItems(objects: SamplerObject[], names: Map<string, string>): SequenceItem[] {
    return objects
        .filter((object) => object.objectType === 'SEQU')
        .map((object) => ({
            id: object.key,
            objectId: object.key,
            name: objectPresentationName(object, names),
            object,
        }));
}

function bankItems(
    objects: SamplerObject[],
    relationships: SamplerRelationship[],
    names: Map<string, string>,
): SampleStructureItem[] {
    return objects.map((object) => ({
        id: object.key,
        objectId: object.key,
        object,
        objectType: 'SBAC',
        name: objectPresentationName(object, names),
        memberCount: relationships.filter(
            (item) => item.sourceObjectId === object.key && item.relationshipType === 'SBAC_SLOT_TO_SBNK',
        ).length,
    }));
}

function sampleItems(
    objects: SamplerObject[],
    banks: SamplerObject[],
    relationships: SamplerRelationship[],
    names: Map<string, string>,
): SampleStructureItem[] {
    return objects
        .filter((object) => object.objectType === 'SBNK')
        .map((object) => {
            const sampleBankObjects = relationships
                .filter((item) => item.targetObjectId === object.key && item.relationshipType === 'SBAC_SLOT_TO_SBNK')
                .map((item) => banks.find((candidate) => candidate.key === item.sourceObjectId))
                .filter((bank): bank is SamplerObject => bank !== undefined);
            const bankNames = sampleBankObjects.map((bank) => objectPresentationName(bank, names));
            return {
                id: object.key,
                objectId: object.key,
                object,
                objectType: 'SBNK',
                name: objectPresentationName(object, names),
                sampleBankObjectIds: sampleBankObjects.map((bank) => bank.key),
                membershipLabel:
                    bankNames.length === 0
                        ? 'Standalone'
                        : bankNames.length === 1
                          ? `Sample Bank: ${bankNames[0]}`
                          : `${bankNames.length} Sample Banks`,
            };
        });
}

function noteName(key: number): string {
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    return `${names[key % 12]}${Math.floor(key / 12) - 2}`;
}
