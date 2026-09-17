import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from './transport';
import { copyObjectDetailToClipboard, serializeObjectDetail } from './objectDetailClipboard';

const detail: ObjectDetail = {
    schemaVersion: 1,
    image: { imageId: 'image-1', revision: 3, format: 'sfs' },
    object: {
        id: 'SMPL:1',
        key: 'SMPL:1',
        type: 'SMPL',
        name: 'Kick',
        format: 'current',
        partitionIndex: 0,
        scopeKey: 'partition:0/volume:1',
        sfsId: 1,
        storedSizeBytes: 512,
        placementResolution: 'MISSING',
        placement: null,
        placementCandidates: [],
        descriptor: {
            dataOffsetBytes: 0,
            groupLabel: 'SMPL',
            groupLabelBasis: 'directory',
            groupLabelStatus: 'exact',
            logicalPath: 'Kick',
            rawGroup: 'SMPL',
            rawVolume: '',
            scopeKey: 'partition:0/volume:1',
            volumeLabel: 'Volume',
            volumeLabelBasis: 'directory',
            volumeLabelStatus: 'exact',
        },
        header: {
            headerSizeBytes: 46,
            layoutSelector0x14: 0,
            name: 'Kick',
            payloadBytes0x1c: 338,
            payloadBytes0x20: 0,
            payloadOffset0x24: 174,
            rawPrefixHex: '534d504c',
            rawType: 'SMPL',
            recordSizeOrHeaderUsed0x18: 512,
        },
        decoded: { kind: 'waveData', sampleRate: 44_100 },
        omissions: [{ kind: 'AUDIO_PCM', reason: 'Binary audio content is omitted', sizeBytes: 338 }],
    },
    relationships: [],
};

describe('object detail clipboard', () => {
    it('serializes the complete object detail as stable, readable JSON', () => {
        const serialized = serializeObjectDetail(detail);

        expect(serialized).toBe(`${JSON.stringify(detail, null, 2)}\n`);
        expect(JSON.parse(serialized)).toEqual(detail);
    });

    it('writes the serialized detail in one clipboard operation', async () => {
        const writeText = vi.fn().mockResolvedValue(undefined);

        await copyObjectDetailToClipboard(detail, { writeText });

        expect(writeText).toHaveBeenCalledOnce();
        expect(writeText).toHaveBeenCalledWith(`${JSON.stringify(detail, null, 2)}\n`);
    });

    it('reports unavailable clipboard access', async () => {
        await expect(copyObjectDetailToClipboard(detail, undefined)).rejects.toThrow('Clipboard access is unavailable');
    });
});
