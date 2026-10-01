import { describe, expect, it } from 'vitest';
import manifest from '../../../../tests/fixtures/manifests/alteration/audio-import.json';
import { audioImportRequest } from './httpImportOperations';
import { clientUploadLocation, serverFileLocation } from './storageLocations';
import type { AudioImportItem } from './transport';

describe('audio import native manifest contract', () => {
    it.each(
        [false, true].flatMap((createVolume) =>
            [false, true].flatMap((bank) =>
                (['A3000_188', 'A4000_A5000_224'] as const).map((sampleFormat) => ({
                    createVolume,
                    bank,
                    sampleFormat,
                })),
            ),
        ),
    )(
        'matches the native-tested manifest ($createVolume, $bank, $sampleFormat)',
        ({ createVolume, bank, sampleFormat }) => {
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
                {
                    sampleFormat,
                    grouping: bank ? { kind: 'SAMPLE_BANK', sampleBankName: 'Imported Bank' } : { kind: 'SAMPLES' },
                },
            );
            expect(result.manifest.inline).toEqual({
                ...manifest,
                operations: manifest.operations
                    .map((op) =>
                        op.type === 'insert_sbnk'
                            ? { ...op, sample: { ...op.sample, storage_format: sampleFormat.toLowerCase() } }
                            : op.type === 'insert_sbac'
                              ? {
                                    ...op,
                                    sample_bank: { ...op.sample_bank, storage_format: sampleFormat.toLowerCase() },
                                }
                              : op,
                    )
                    .filter(
                        (op) => (createVolume || op.type !== 'insert_volume') && (bank || op.type !== 'insert_sbac'),
                    ),
            });
            expect(result.inputBindings).toEqual([
                { manifestPath: 'audio/import-0', input: { uploadRef: { uploadId: 'mono' } } },
                {
                    manifestPath: 'audio/import-1',
                    input: { fileRef: { rootId: 'workspace', relativePath: 'stereo.wav' } },
                },
            ]);
        },
    );
});
