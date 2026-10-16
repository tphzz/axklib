import type { components } from '../lib/generated/axklibApiV1';
import type { ImageTransport, SamplerObject } from '../lib/transport';
import type { Program } from '../lib/types';
import { nativeDocumentFixture, nativeObjects, tx16wDocument } from './nativeDocumentFixture';
import { programFormatFixture } from './programFormatFixture';

const kinds = ['performance', 'voice', 'timbre', 'setup'] as const;
export const longCollectionName = 'Long collection name '.repeat(18).trim();
export const collectionObjects: components['schemas']['ImageObjectItem'][] = [
    ...nativeObjects,
    ...kinds.flatMap((type) =>
        Array.from({ length: 40 }, (_, index) => ({
            ...nativeObjects[0]!,
            id: `${type}-${index}`,
            type,
            name: index === 0 ? '6' : index === 1 ? longCollectionName : `${type} ${index}`,
            sizeBytes: index === 1 ? Number.MAX_SAFE_INTEGER : 146,
        })),
    ),
];
export const collectionDocument = { ...tx16wDocument, objectCount: collectionObjects.length };

function details(type: string, slot: number): components['schemas']['Tx16wObjectDetails'] {
    switch (type) {
        case 'performance':
            return { type, slot, voices: [] };
        case 'voice':
            return { type, slot, regions: [] };
        case 'timbre':
            return { type, slot, waveSlot: 1, rootKey: 60 };
        case 'setup':
            return { type, writeProtected: false, waves: [] };
        default:
            throw new Error(`Unexpected collection fixture kind: ${type}`);
    }
}

export function nativeCollectionFixture(): ImageTransport {
    const base = nativeDocumentFixture();
    return {
        ...base,
        nativeObjects: async () => collectionObjects,
        nativeObjectDetail: async (sessionId, objectId) => {
            const object = collectionObjects.find((item) => item.id === objectId);
            if (!object) throw new Error(`Unknown collection fixture object: ${objectId}`);
            if (object.type === 'wave') return base.nativeObjectDetail(sessionId, objectId);
            return {
                imageId: 'image-1',
                documentId: collectionDocument.id,
                formatId: 'yamaha.tx16w',
                profile: 'yamaha-native',
                revision: 1,
                objectId,
                type: object.type,
                name: object.name,
                locations: [],
                details: details(object.type, Number(objectId.split('-')[1]) + 1),
            };
        },
    };
}

export const collectionPrograms: Program[] = Array.from({ length: 40 }, (_, index) => {
    const object: SamplerObject = {
        key: `program-${index}`,
        objectType: 'PROG',
        name: String(index + 1),
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: 'PROG',
        objectEncoding: 'current',
        directoryEntryName: String(index + 1),
        sfsId: index + 1,
        storedSizeBytes: 912,
        sizeWithDependenciesBytes: 520192,
        sampleRate: 0,
        rootKey: 0,
        storedFrameCount: 0,
        waveStartFrame: 0,
        waveLengthFrames: 0,
        sampleWidthBytes: 0,
        storageState: 'COMPLETE',
        programFormat: programFormatFixture(true),
    };
    return {
        id: object.key,
        objectId: object.key,
        slot: String(index + 1).padStart(3, '0'),
        programNumber: index + 1,
        name: index === 0 ? '6' : index === 1 ? longCollectionName : `Program ${index}`,
        object,
        assignmentSummary:
            index === 1 ? '1 assignment containing a long metadata description '.repeat(20) : '1 assignment',
    };
});
