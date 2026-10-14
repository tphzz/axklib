import type { components } from '../lib/generated/axklibApiV1';
import type { ImageTransport, NativeDocumentSummary } from '../lib/transport';
import { nativeWorkspaceFixture } from './nativeWorkspaceFixture';
import { storagePickerFixture } from './storagePickerFixture';

export const tx16wDocument: NativeDocumentSummary = {
    ...nativeWorkspaceFixture().documents[0]!,
    id: 'document-tx16w',
    name: 'TX16W disk set',
    formatId: 'yamaha.tx16w',
    profile: 'yamaha-native',
    format: 'yamaha.tx16w',
    completeness: 'INCOMPLETE',
    companions: {
        canAttach: true,
        canSearchNearby: true,
        requirements: [{ name: 'WAVE.W01', state: 'MISSING', objectId: null }],
    },
    sourceMemberIds: ['source-2', 'source-3'],
    objectCount: 60,
    relationshipCount: 1,
};
export const nativeObjects: components['schemas']['ImageObjectItem'][] = Array.from({ length: 60 }, (_, index) => ({
    id: `wave-${index}`,
    type: 'wave',
    name: `Wave ${String(index).padStart(2, '0')}`,
    sizeBytes: 512,
    format: 'yamaha.tx16w',
    categoryName: '',
    entryName: `WAVE${index}.W01`,
    partitionIndex: null,
    partitionName: '',
    volumeName: '',
    sizeWithDependenciesBytes: null,
    programFormat: null,
    sampleFormat: null,
    sequence: null,
    waveform: null,
}));
export function nativeDocumentFixture(): ImageTransport {
    return {
        ...storagePickerFixture(true),
        nativeObjects: async () => nativeObjects,
        nativeRelationships: async () => [
            {
                id: 'edge-1',
                sourceObjectId: 'wave-20',
                targetObjectId: null,
                candidateObjectIds: ['wave-21', 'wave-22'],
                type: 'native-reference',
                quality: 'AMBIGUOUS',
                basis: 'Multiple source occurrences',
                notes: [],
                assignmentIndex: null,
                assignmentName: '',
                assignmentState: '',
                receiveChannelDisplay: '',
            },
        ],
        nativeObjectDetail: async (
            _session: number,
            objectId: string,
        ): Promise<components['schemas']['Tx16wObjectDetail']> => ({
            imageId: 'image-1',
            documentId: tx16wDocument.id,
            formatId: 'yamaha.tx16w',
            profile: 'yamaha-native',
            revision: 1,
            objectId,
            name: nativeObjects.find((item) => item.id === objectId)!.name,
            type: 'wave',
            locations: [{ memberId: 'source-2', rootId: 'fat12', entryId: objectId, offset: 0, length: 512 }],
            details: { type: 'wave', sampleRate: 33333, attackFrames: 128, repeatFrames: 0, looped: false },
        }),
    } as unknown as ImageTransport;
}
