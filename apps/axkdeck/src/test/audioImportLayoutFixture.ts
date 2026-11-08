import { ImportCompletion } from '../features/import/importCompletion.svelte';
import { JobController } from '../features/jobs/actions';
import { defaultAudioSamplerSettings } from '../lib/audioImport';
import { serverFileLocation, type InputFileLocation } from '../lib/storageLocations';
import type { AudioImportItem, AudioImportOptions, AudioSourceInfo, ImageTransport } from '../lib/transport';

export function createAudioImportLayoutFixture(scenario: string) {
    const names =
        scenario === 'many'
            ? Array.from({ length: 16 }, (_, index) => `Loop_${index + 1}.wav`)
            : scenario === 'long'
              ? ['WWWWWWWWWWWWWWWW.wav']
              : [
                    'DNBS_drum_loop_172_04.flac',
                    'DNBS_drum_loop_172_04.wav',
                    'DNBS_drum_loop_172_06.wav',
                    'DNBS_drum_loop_172_08.wav',
                    'DNBS_drum_loop_172_12.wav',
                    'DNBS_drum_loop_172_16.wav',
                ].slice(0, scenario === 'normal' ? 6 : 3);
    let releaseInspection = () => {};
    const inspection =
        scenario === 'inspecting' ? new Promise<void>((resolve) => (releaseInspection = resolve)) : Promise.resolve();
    let releaseCommit: (value: boolean) => void = () => {};
    let refreshed = 0;
    const submissions: { items: AudioImportItem[]; options: AudioImportOptions; warnings: string[] }[] = [];
    const transport = {
        audioImportCapabilities: async () => ({
            supportedSampleRates: [22050, 44100, 48000],
            defaultUnsupportedSampleRate: 44100,
            supportedOutputSampleWidthsBits: [16],
            sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8',
            maximumUploads: 1024,
        }),
        inspectAudio: async (source: InputFileLocation, rate = 44100): Promise<AudioSourceInfo> => {
            await inspection;
            const channels = scenario === 'mono' ? 1 : 2;
            return {
                sourceFormat: source.displayName.endsWith('.flac') ? 'FLAC' : 'WAV',
                sourceSubtype: 'PCM_24',
                channels,
                frameCount: 246272,
                sourceSampleRate: 44100,
                outputSampleRate: rate,
                sourceSampleWidthBits: 24,
                outputSampleWidthBits: 16,
                durationSeconds: 246272 / 44100,
                resampled: rate !== 44100,
                quantized: true,
                sampleWidthConverted: true,
                ditherAlgorithm: 'axk-tpdf-pcg32-v1',
                projectedOutputFrameCount: 246272,
                projectedOutputBytesPerChannel: 492544,
                projectedOutputBytesTotal: 492544 * channels,
                maximumOutputFrameCountPerChannel: 1 << 24,
                maximumOutputBytesPerChannel: 32 * 1024 * 1024,
                samplerDefaults: {
                    ...defaultAudioSamplerSettings,
                    ...(scenario === 'loop' || scenario === 'long' ? { rootKey: 1, fineTuneCents: -63 } : {}),
                    ...(scenario === 'loop'
                        ? { loopMode: 1 as const, loopStartFrame: 0, loopLengthFrames: 246272 }
                        : {}),
                },
                valid: scenario !== 'invalid',
                issues:
                    scenario === 'invalid'
                        ? [
                              {
                                  code: 'fixture',
                                  message: 'Unsupported source audio. The full inspection reason must remain visible.',
                                  fatal: true,
                              },
                          ]
                        : [],
            };
        },
        downloadFile: async () => {
            throw new Error(
                'The selected output device is unavailable. ' +
                    'The existing import parameters and source selection are retained. '.repeat(8),
            );
        },
        waitForJob: async () => ({
            jobId: 1,
            status: 'completed',
            result: {
                kind: 'ALTERATION',
                operations: [],
                warnings: scenario === 'warnings' ? [{ message: 'Conversion warning retained for review' }] : [],
            },
        }),
    } as unknown as ImageTransport;
    const completion = new ImportCompletion(transport, new JobController(transport));
    async function commit(items: AudioImportItem[], options: AudioImportOptions, warnings: string[]) {
        submissions.push({ items, options, warnings });
        if (scenario === 'busy') return new Promise<boolean>((resolve) => (releaseCommit = resolve));
        return completion.run(
            async () => ({ jobId: 1, status: 'queued', kind: 'alter' }),
            async () => {
                if (scenario === 'refresh-failed' && refreshed++ === 0)
                    throw new Error('Fixture refresh failed after the committed write');
            },
        );
    }
    return {
        transport,
        completion,
        commit,
        submissions,
        releaseInspection,
        finishCommit: (result: boolean) => releaseCommit(result),
        files: names.map((name) => serverFileLocation({ rootId: 'fixture', relativePath: name }, name)),
        existingSampleNames: scenario === 'conflict' ? ['DNBS_drum_loop_1'] : [],
    };
}
