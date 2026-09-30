import type { components } from './generated/axklibApiV1';
import type { ImportPartitionOption } from '../features/import/packageDestinations';
import type { ImageSessionPackageImportPlan } from './transport';
import type { InputFileLocation } from './storageLocations';
import type { FilesystemEdit } from './filesystem';
import type {
    AudioImportItem,
    AudioImportOptions,
    VolumeImportDestination,
    AudioImportTarget,
    SequenceImportItem,
    SequenceSystemExclusivePolicy,
    Tx16wImportMode,
    JobState,
    SampleBankCreation,
    SampleBankAssignment,
} from './transport';

export type CapacityPolicy = components['schemas']['VolumeCapacityPolicy'];
export type CapacityAdmission = components['schemas']['VolumeCapacityAdmission'];
export type CapacityImport =
    | { kind: 'AUDIO'; target: VolumeImportDestination; items: AudioImportItem[]; options: AudioImportOptions }
    | {
          kind: 'SEQUENCE';
          target: VolumeImportDestination;
          items: SequenceImportItem[];
          systemExclusivePolicy: SequenceSystemExclusivePolicy;
      }
    | { kind: 'TX16W'; target: AudioImportTarget; sources: InputFileLocation[]; importMode: Tx16wImportMode }
    | { kind: 'FILES'; expectedRevision: number; edits: FilesystemEdit[] };

export interface ImportMutationTransport {
    inspectImportCapacity(
        sessionId: number,
        request: CapacityImport,
        policy: CapacityPolicy,
    ): Promise<CapacityAdmission>;
    startAudioImport(
        sessionId: number,
        target: VolumeImportDestination,
        items: AudioImportItem[],
        options: AudioImportOptions,
        policy?: CapacityPolicy,
    ): Promise<JobState>;
    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState>;
    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState>;
    startSequenceImport(
        sessionId: number,
        target: VolumeImportDestination,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
        policy?: CapacityPolicy,
    ): Promise<JobState>;
    startTx16wDiskSetImport(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
        policy?: CapacityPolicy,
    ): Promise<JobState>;
}

export function capacityFailure(admission: CapacityAdmission): string {
    for (const report of admission.reports) {
        const profile = report.profiles.find((profile) => profile.target === admission.target);
        if (profile && profile.status !== 'FITS')
            return `${report.volumeName}: ${profile.reasons[0]?.message ?? 'Volume does not fit the selected sampler.'}`;
    }
    return 'Volume does not fit the selected sampler.';
}

type Conflict = ImageSessionPackageImportPlan['conflicts'][number];

export function isImportSpaceConflict(conflict: Conflict): boolean {
    return conflict.code === 'SFS_CLUSTER_EXHAUSTED';
}

export function importCapacityGroups(conflicts: Conflict[], partitions: ImportPartitionOption[]) {
    const groups = new Map<number | null, { partitionIndex: number | null; name: string; conflicts: Conflict[] }>();
    for (const conflict of conflicts) {
        if (!isImportSpaceConflict(conflict)) continue;
        const index = conflict.partitionIndex;
        let group = groups.get(index);
        if (!group) {
            group = {
                partitionIndex: index,
                name:
                    partitions.find((partition) => partition.partitionIndex === index)?.name ||
                    (index === null ? 'the destination partition' : `Partition ${index + 1}`),
                conflicts: [],
            };
            groups.set(index, group);
        }
        group.conflicts.push(conflict);
    }
    return [...groups.values()];
}
