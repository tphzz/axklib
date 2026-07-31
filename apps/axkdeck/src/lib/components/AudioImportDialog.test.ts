import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import { clientUploadLocation, serverFileLocation } from '../storageLocations';
import type { ClientUploadSource } from '../clientUploadSource';
import type { AudioImportCapabilities, AudioSourceInfo, ImageTransport } from '../transport';
import AudioImportDialog from './AudioImportDialog.svelte';

const capabilities: AudioImportCapabilities = {
    supportedSampleRates: [22_050, 44_100, 48_000],
    defaultUnsupportedSampleRate: 44_100,
    supportedOutputSampleWidthsBits: [16],
    sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8',
};

function sourceInfo(overrides: Partial<AudioSourceInfo> = {}): AudioSourceInfo {
    return {
        sourceFormat: 'FLAC',
        sourceSubtype: 'PCM_24',
        channels: 2,
        frameCount: 96_000,
        sourceSampleRate: 48_000,
        outputSampleRate: 48_000,
        sourceSampleWidthBits: 24,
        outputSampleWidthBits: 16,
        durationSeconds: 2,
        resampled: false,
        quantized: true,
        sampleWidthConverted: true,
        ditherAlgorithm: 'axk-tpdf-pcg32-v1',
        projectedOutputFrameCount: 96_000,
        projectedOutputBytesPerChannel: 192_000,
        projectedOutputBytesTotal: 384_000,
        maximumOutputFrameCountPerChannel: 1 << 24,
        maximumOutputBytesPerChannel: 32 * 1024 * 1024,
        samplerDefaults: {
            rootKey: 60,
            fineTuneCents: 0,
            keyLow: 0,
            keyHigh: 127,
            velocityLow: 0,
            velocityHigh: 127,
            loopMode: 4,
            loopStartFrame: 0,
            loopLengthFrames: 0,
            pitchSource: 'DEFAULT',
            rangeSource: 'DEFAULT',
            loopSource: 'DEFAULT',
        },
        valid: true,
        issues: [],
        ...overrides,
    };
}

function transport(): ImageTransport {
    return {
        audioImportCapabilities: vi.fn().mockResolvedValue(capabilities),
        uploadClientFile: vi.fn(async (file: ClientUploadSource, _kind, onProgress) => {
            onProgress?.(file.size, file.size);
            return clientUploadLocation({ uploadId: 'audio-stereo' }, 'AUDIO', file.name);
        }),
        inspectAudio: vi.fn().mockResolvedValue(sourceInfo()),
        releaseClientUpload: vi.fn().mockResolvedValue(undefined),
    } as unknown as ImageTransport;
}

describe('AudioImportDialog', () => {
    it('offers the shared workspace and local source choices before staging files', async () => {
        const onchooseworkspace = vi.fn();
        const onchooselocal = vi.fn();
        render(AudioImportDialog, {
            props: {
                transport: transport(),
                files: [],
                target: { partitionIndex: 0, volumeName: 'My Volume' },
                existingSampleNames: [],
                existingWaveformNames: [],
                onchooseworkspace,
                onchooselocal,
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Choose audio files')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));

        expect(onchooseworkspace).toHaveBeenCalledOnce();
        expect(onchooselocal).toHaveBeenCalledOnce();
    });

    it('inspects workspace files directly and applies the shared collision-free naming policy', async () => {
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                sourceSampleRate: 11_000,
                outputSampleRate: 44_100,
                sourceSampleWidthBits: 16,
                sampleWidthConverted: false,
                projectedOutputBytesTotal: 192_000,
            }),
        );
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'audio/16bit_11k.wav' },
            'Yamaha/audio/16bit_11k.wav',
        );
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [workspaceFile],
                target: { partitionIndex: 0, volumeName: 'My Volume' },
                existingSampleNames: ['16bit_11k'],
                existingWaveformNames: ['16bit_11k'],
                oncommit,
                oncancel: vi.fn(),
                onchooseworkspace: vi.fn(),
                onchooselocal: vi.fn(),
            },
        });

        expect(await screen.findAllByDisplayValue('16bit_11k 2')).toHaveLength(2);
        expect(imageTransport.uploadClientFile).not.toHaveBeenCalled();
        expect(imageTransport.inspectAudio).toHaveBeenCalledWith(workspaceFile);

        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([
                expect.objectContaining({
                    source: workspaceFile,
                    sampleName: '16bit_11k 2',
                    waveformNames: ['16bit_11k 2'],
                }),
            ]),
        );
        expect(imageTransport.releaseClientUpload).not.toHaveBeenCalled();
    });

    it('aligns mixed mono and stereo files in stable channel columns', async () => {
        const imageTransport = transport();
        imageTransport.uploadClientFile = vi.fn(async (file: ClientUploadSource, _kind, onProgress) => {
            onProgress?.(file.size, file.size);
            return clientUploadLocation({ uploadId: file.name }, 'AUDIO', file.name);
        });
        imageTransport.inspectAudio = vi.fn(async (source) => {
            const channels: 1 | 2 = source.displayName.startsWith('Mono') ? 1 : 2;
            return sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels,
                frameCount: 44_100,
                sourceSampleRate: 44_100,
                outputSampleRate: 44_100,
                sourceSampleWidthBits: 16,
                durationSeconds: 1,
                resampled: false,
                quantized: false,
                sampleWidthConverted: false,
                ditherAlgorithm: '',
                projectedOutputFrameCount: 44_100,
                projectedOutputBytesPerChannel: 88_200,
                projectedOutputBytesTotal: 88_200 * channels,
            });
        });
        const monoFilename = 'Mono voice recording with a deliberately long source filename.wav';
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [
                    new File([new Uint8Array(64)], monoFilename, { type: 'audio/wav' }),
                    new File([new Uint8Array(128)], 'Stereo pad.wav', { type: 'audio/wav' }),
                ],
                target: { partitionIndex: 0, volumeName: 'Mixed' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findAllByDisplayValue('Mono voice recor')).toHaveLength(2);
        expect(screen.getByRole('columnheader', { name: 'Source file' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Target rate' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Sample name' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Wave data (mono/left)' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Wave data (right)' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Root key' })).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'Status' })).toBeTruthy();

        const monoRow = screen.getByTitle(monoFilename).closest('tr');
        expect(monoRow).not.toBeNull();
        expect(within(monoRow!).getByLabelText('Wave data (mono/left)')).toBeTruthy();
        expect(within(monoRow!).getByLabelText('No right wave data').textContent).toBe('—');

        const stereoRow = screen.getByTitle('Stereo pad.wav').closest('tr');
        expect(stereoRow).not.toBeNull();
        expect(within(stereoRow!).getByLabelText('Wave data (mono/left)')).toBeTruthy();
        expect(within(stereoRow!).getByLabelText('Wave data (right)')).toBeTruthy();
    });

    it('reviews stereo names and releases the staged upload after one commit', async () => {
        const imageTransport = transport();
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const oncancel = vi.fn();
        const file = new File([new Uint8Array(512)], 'Stereo piano.flac', { type: 'audio/flac' });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [file],
                target: { partitionIndex: 2, volumeName: 'Keys' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel,
            },
        });

        expect(screen.getByRole('dialog', { name: 'Import audio' })).toBeTruthy();
        expect(await screen.findByDisplayValue('Stereo piano')).toBeTruthy();
        expect(screen.getByDisplayValue('Stereo piano-L')).toBeTruthy();
        expect(screen.getByDisplayValue('Stereo piano-R')).toBeTruthy();
        expect(screen.getByText('FLAC PCM_24 · Stereo · 48,000 Hz · 24 → 16-bit TPDF · 2.00 s')).toBeTruthy();

        await fireEvent.input(screen.getByLabelText('Root key'), { target: { value: '69' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));

        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([
                {
                    source: expect.objectContaining({ reference: { uploadId: 'audio-stereo' } }),
                    sampleName: 'Stereo piano',
                    waveformNames: ['Stereo piano-L', 'Stereo piano-R'],
                    rootKey: 69,
                    fineTuneCents: 0,
                    keyLow: 0,
                    keyHigh: 127,
                    velocityLow: 0,
                    velocityHigh: 127,
                    loopMode: 4,
                    loopStartFrame: 0,
                    loopLengthFrames: 0,
                    targetSampleRate: 48_000,
                },
            ]),
        );
        expect(imageTransport.releaseClientUpload).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { uploadId: 'audio-stereo' } }),
        );
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('reviews WAV sampler metadata and commits the mapped pitch, ranges, and loop', async () => {
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                sourceSampleWidthBits: 16,
                sampleWidthConverted: false,
                quantized: false,
                samplerDefaults: {
                    rootKey: 62,
                    fineTuneCents: -63,
                    keyLow: 12,
                    keyHigh: 96,
                    velocityLow: 8,
                    velocityHigh: 110,
                    loopMode: 1,
                    loopStartFrame: 1_234,
                    loopLengthFrames: 4_321,
                    pitchSource: 'WAV_SMPL',
                    rangeSource: 'WAV_INST',
                    loopSource: 'WAV_SMPL',
                },
                issues: [
                    {
                        code: 'wav_sampler_loop_unsupported',
                        message: 'An additional WAV sampler loop was ignored.',
                        fatal: false,
                    },
                ],
            }),
        );
        const oncommit = vi.fn().mockResolvedValue(undefined);
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [new File([new Uint8Array(128)], 'Mapped.wav', { type: 'audio/wav' })],
                target: { partitionIndex: 0, volumeName: 'Mapped' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByLabelText('Sampler settings for Mapped.wav')).toBeTruthy();
        expect(screen.getByDisplayValue('-63')).toBeTruthy();
        expect(screen.getByDisplayValue('1234')).toBeTruthy();
        expect(screen.getByText('Pitch: WAV smpl')).toBeTruthy();
        expect(screen.getByText('Ranges: WAV inst')).toBeTruthy();
        expect(screen.getByText('An additional WAV sampler loop was ignored.')).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([
                expect.objectContaining({
                    rootKey: 62,
                    fineTuneCents: -63,
                    keyLow: 12,
                    keyHigh: 96,
                    velocityLow: 8,
                    velocityHigh: 110,
                    loopMode: 1,
                    loopStartFrame: 1_234,
                    loopLengthFrames: 4_321,
                }),
            ]),
        );
    });

    it('revalidates one file when its target sample rate changes', async () => {
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn(async (_source, targetSampleRate) =>
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                frameCount: 96_000,
                sourceSampleRate: 96_000,
                outputSampleRate: targetSampleRate ?? 44_100,
                sourceSampleWidthBits: 16,
                durationSeconds: 1,
                resampled: true,
                quantized: true,
                sampleWidthConverted: false,
                projectedOutputFrameCount: targetSampleRate ?? 44_100,
                projectedOutputBytesPerChannel: (targetSampleRate ?? 44_100) * 2,
                projectedOutputBytesTotal: (targetSampleRate ?? 44_100) * 2,
            }),
        );
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const file = new File([new Uint8Array(128)], 'Unsupported rate.wav', { type: 'audio/wav' });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [file],
                target: { partitionIndex: 0, volumeName: 'Rates' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        const selector = await screen.findByRole('combobox', {
            name: `Target sample rate for ${file.name}`,
        });
        expect((selector as HTMLSelectElement).value).toBe('44100');

        await fireEvent.change(selector, { target: { value: '22050' } });
        await waitFor(() => expect(imageTransport.inspectAudio).toHaveBeenLastCalledWith(expect.anything(), 22_050));
        await waitFor(() => expect((selector as HTMLSelectElement).value).toBe('22050'));
        expect(screen.getByText('WAV PCM_16 · Mono · 96,000 Hz · 16-bit · resampled TPDF · 1.00 s')).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([
                expect.objectContaining({
                    targetSampleRate: 22_050,
                }),
            ]),
        );
    });

    it('waits for in-flight inspection before releasing uploads on cancel', async () => {
        let finishInspection!: (value: AudioSourceInfo) => void;
        const inspection = new Promise<AudioSourceInfo>((resolve) => {
            finishInspection = resolve;
        });
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn(() => inspection);
        const oncancel = vi.fn();
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [new File([new Uint8Array(64)], 'voice.wav', { type: 'audio/wav' })],
                target: { partitionIndex: 0, volumeName: 'Voice' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel,
            },
        });

        await waitFor(() => expect(imageTransport.inspectAudio).toHaveBeenCalledOnce());
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
        expect(oncancel).not.toHaveBeenCalled();
        finishInspection(
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                frameCount: 64,
                sourceSampleRate: 44_100,
                outputSampleRate: 44_100,
                sourceSampleWidthBits: 16,
                durationSeconds: 64 / 44_100,
                resampled: false,
                quantized: false,
                sampleWidthConverted: false,
                ditherAlgorithm: '',
                projectedOutputFrameCount: 64,
                projectedOutputBytesPerChannel: 128,
                projectedOutputBytesTotal: 128,
            }),
        );

        await waitFor(() => expect(imageTransport.releaseClientUpload).toHaveBeenCalledOnce());
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('removes a rejected file while retaining valid staged files', async () => {
        const imageTransport = transport();
        imageTransport.uploadClientFile = vi.fn(async (file: ClientUploadSource, _kind, onProgress) => {
            onProgress?.(file.size, file.size);
            return clientUploadLocation({ uploadId: file.name }, 'AUDIO', file.name);
        });
        imageTransport.inspectAudio = vi.fn(async (source) => {
            const invalid = source.displayName === 'Too large.wav';
            return sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1 as const,
                frameCount: invalid ? 17_825_792 : 44_100,
                sourceSampleRate: 44_100,
                outputSampleRate: 44_100,
                sourceSampleWidthBits: 16,
                durationSeconds: invalid ? 404.21 : 1,
                resampled: false,
                quantized: false,
                sampleWidthConverted: false,
                ditherAlgorithm: '',
                projectedOutputFrameCount: invalid ? 17_825_792 : 44_100,
                projectedOutputBytesPerChannel: invalid ? 34 * 1024 * 1024 : 88_200,
                projectedOutputBytesTotal: invalid ? 34 * 1024 * 1024 : 88_200,
                valid: !invalid,
                issues: invalid
                    ? [
                          {
                              code: 'wave_data_channel_too_large',
                              message:
                                  'Converted Wave Data is 34.0 MiB per channel; A-series hardware supports at most 32 MiB per channel.',
                              fatal: true,
                          },
                      ]
                    : [],
            });
        });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [
                    new File([new Uint8Array(64)], 'Too large.wav', { type: 'audio/wav' }),
                    new File([new Uint8Array(64)], 'Valid.wav', { type: 'audio/wav' }),
                ],
                target: { partitionIndex: 0, volumeName: 'Mixed' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByText(/Converted Wave Data is 34.0 MiB per channel/)).toBeTruthy();
        expect(await screen.findByText('Fits · 87 KiB')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Remove Too large.wav' }));

        await waitFor(() => expect(screen.queryByTitle('Too large.wav')).toBeNull());
        expect(screen.getByTitle('Valid.wav')).toBeTruthy();
        expect(imageTransport.releaseClientUpload).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { uploadId: 'Too large.wav' } }),
        );
        expect((screen.getByRole('button', { name: 'Import 1 file' }) as HTMLButtonElement).disabled).toBe(false);
    });
});
