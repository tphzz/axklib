import { describe, expect, it } from 'vitest';
import manifest from '../../../../tests/fixtures/manifests/alteration/audio-import.json';
import { audioImportRequest } from './httpImportOperations';
import { clientUploadLocation, serverFileLocation } from './storageLocations';
import type { AudioImportItem } from './transport';

describe('audio import native manifest contract', () => {
    it.each([
        [false, false],
        [false, true],
        [true, false],
        [true, true],
    ])('matches the native-tested manifest (new volume=%s, Sample Bank=%s)', (createVolume, bank) => {
        const items: AudioImportItem[] = [
            {
                source: clientUploadLocation({ uploadId: 'mono' }, 'AUDIO', 'mono.wav'),
                sampleName: 'Mono',
                waveformNames: ['Mono Wave'],
                rootKey: 60,
                fineTuneCents: 0,
                keyLow: 0,
                keyHigh: 127,
                velocityLow: 0,
                velocityHigh: 127,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                targetSampleRate: 44_100,
            },
            {
                source: serverFileLocation({ rootId: 'workspace', relativePath: 'stereo.wav' }),
                sampleName: 'Stereo',
                waveformNames: ['Stereo-L', 'Stereo-R'],
                rootKey: 69,
                fineTuneCents: -5,
                keyLow: 12,
                keyHigh: 108,
                velocityLow: 4,
                velocityHigh: 120,
                loopMode: 1,
                loopStartFrame: 8,
                loopLengthFrames: 32,
                targetSampleRate: 44_100,
            },
        ];
        const result = audioImportRequest(
            'image',
            1,
            { kind: createVolume ? 'CREATE_VOLUME' : 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            items,
            bank ? { kind: 'SAMPLE_BANK', sampleBankName: 'Imported Bank' } : { kind: 'SAMPLES' },
        );
        expect(result.manifest.inline).toEqual({
            ...manifest,
            operations: manifest.operations.filter(
                (op) => (createVolume || op.type !== 'insert_volume') && (bank || op.type !== 'insert_sbac'),
            ),
        });
        expect(result.inputBindings).toEqual([
            { manifestPath: 'audio/import-0', input: { uploadRef: { uploadId: 'mono' } } },
            { manifestPath: 'audio/import-1', input: { fileRef: { rootId: 'workspace', relativePath: 'stereo.wav' } } },
        ]);
    });
});
