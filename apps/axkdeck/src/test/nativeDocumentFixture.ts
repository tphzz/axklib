import type { components } from '../lib/generated/axklibApiV1';
import type { ImageTransport, NativeDocumentSummary } from '../lib/transport';
import { nativeWorkspaceFixture } from './nativeWorkspaceFixture';
import { storagePickerFixture } from './storagePickerFixture';
import { fixtureBins, fixtureFrames, fixtureWave } from './sampleEditorAudio';

export const nativeSampleRate = 33333;
function nativeWave(): ArrayBuffer {
    const bytes = fixtureWave();
    const header = new DataView(bytes);
    header.setUint32(24, nativeSampleRate, true);
    header.setUint32(28, nativeSampleRate * 2, true);
    for (let offset = 44; offset < bytes.byteLength; offset += 2)
        header.setInt16(offset, header.getInt16(offset, true) & ~15, true);
    return bytes;
}

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
    const identity = { imageId: 'image-1', documentId: tx16wDocument.id, revision: 1 };
    return {
        ...storagePickerFixture(true),
        audioIdentity: () => ({ ...identity }),
        preview: async (_session: number, objectId: string, count: number) => ({
            ...identity,
            objectId,
            warnings: ['Sample rate is uncertain; audition uses 33,333 Hz. Duration is approximate.'],
            lanes: [
                {
                    sourceObjectId: objectId,
                    role: 'MONO',
                    sampleRate: nativeSampleRate,
                    sampleRateApproximate: true,
                    sampleWidthBytes: 2,
                    nativeBitDepth: 12,
                    storedFrameCount: fixtureFrames,
                    playbackStartFrame: 0,
                    playbackLengthFrames: fixtureFrames,
                    loopStartFrame: 0,
                    loopLengthFrames: 0,
                    bins: fixtureBins(Math.min(count, fixtureFrames)),
                },
            ],
        }),
        prepareAuditionBundle: async (_session: number, objectIds: readonly string[]) => ({
            ...identity,
            auditionId: 'native-audition',
            contentSizeBytes: objectIds.length * (44 + fixtureFrames * 2),
            clips: objectIds.map((objectId, index) => ({
                objectId,
                direction: 'FORWARD',
                looping: false,
                warnings: [],
                lanes: [
                    {
                        role: 'MONO',
                        sourceObjectId: objectId,
                        sampleRate: nativeSampleRate,
                        sampleWidthBytes: 2,
                        frameCount: fixtureFrames,
                        contentOffsetBytes: index * (44 + fixtureFrames * 2),
                        wavSizeBytes: 44 + fixtureFrames * 2,
                        loopStartFrame: 0,
                        loopLengthFrames: 0,
                    },
                ],
            })),
        }),
        readAuditionContent: async (_auditionId: string, size: number) => {
            const wave = new Uint8Array(nativeWave());
            const result = new Uint8Array(size);
            for (let offset = 0; offset < size; offset += wave.length) result.set(wave, offset);
            return result.buffer;
        },
        deleteAudition: async () => {},
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
            details: {
                type: 'wave',
                sampleRate: nativeSampleRate,
                nativeBitDepth: 12,
                sampleRateApproximate: true,
                attackFrames: fixtureFrames,
                repeatFrames: 0,
                looped: false,
            },
        }),
    } as unknown as ImageTransport;
}
