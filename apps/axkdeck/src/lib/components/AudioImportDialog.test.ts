import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import { clientUploadLocation, serverFileLocation } from '../storageLocations';
import type { ClientUploadSource } from '../clientUploadSource';
import type { AudioImportCapabilities, AudioSourceInfo, ImageTransport } from '../transport';
import AudioImportDialog from './AudioImportDialog.svelte';

const audioImportRowsSource = readFileSync(resolve(process.cwd(), 'src/lib/components/AudioImportRows.svelte'), 'utf8');
const audioSamplerSettingsSource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/AudioSamplerSettings.svelte'),
    'utf8',
);

const capabilities: AudioImportCapabilities = {
    supportedSampleRates: [22_050, 44_100, 48_000],
    defaultUnsupportedSampleRate: 44_100,
    supportedOutputSampleWidthsBits: [16],
    sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8',
    maximumUploads: 1024,
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
    it('keeps sampler fields aligned while reserving clearance for the scrollbar', () => {
        expect(audioSamplerSettingsSource).toContain('grid-template-columns: repeat(7, minmax(0, 1fr));');
        expect(audioSamplerSettingsSource).not.toContain('repeat(auto-fit');
        expect(audioImportRowsSource).toContain('padding-right: 12px;');
        expect(audioImportRowsSource).toContain('scrollbar-gutter: stable;');
    });

    it('imports inspected Samples into a newly named Sample Bank', async () => {
        const oncommit = vi.fn().mockResolvedValue(undefined);
        render(AudioImportDialog, {
            props: {
                transport: transport(),
                files: [new File([new Uint8Array(64)], 'Bass.wav', { type: 'audio/wav' })],
                target: { partitionIndex: 0, volumeName: 'Sounds' },
                existingSampleNames: [],
                existingSampleBankNames: ['Existing'],
                existingWaveformNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        await screen.findByDisplayValue('Bass');
        await fireEvent.change(screen.getByRole('combobox', { name: 'Import mode' }), {
            target: { value: 'SAMPLE_BANK' },
        });
        const name = screen.getByRole('textbox', { name: 'Sample Bank name' });
        await fireEvent.input(name, { target: { value: 'existing' } });
        expect(screen.getByText('Sample Bank name already exists: existing')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Import 1 file' }) as HTMLButtonElement).disabled).toBe(true);

        await fireEvent.input(name, { target: { value: 'Bass Bank' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([expect.objectContaining({ sampleName: 'Bass' })], {
                kind: 'SAMPLE_BANK',
                sampleBankName: 'Bass Bank',
            }),
        );
    });

    it('rejects an oversized local selection before staging any uploads', async () => {
        const imageTransport = transport();
        imageTransport.audioImportCapabilities = vi.fn().mockResolvedValue({
            ...capabilities,
            maximumUploads: 2,
        });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [
                    new File([new Uint8Array(64)], 'First.wav', { type: 'audio/wav' }),
                    new File([new Uint8Array(64)], 'Second.wav', { type: 'audio/wav' }),
                    new File([new Uint8Array(64)], 'Third.wav', { type: 'audio/wav' }),
                ],
                target: { partitionIndex: 0, volumeName: 'Bulk' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(
            await screen.findByText('This server can stage at most 2 local files at once; 3 were selected.'),
        ).toBeTruthy();
        expect(imageTransport.uploadClientFile).not.toHaveBeenCalled();
    });

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
            expect(oncommit).toHaveBeenCalledWith(
                [
                    expect.objectContaining({
                        source: workspaceFile,
                        sampleName: '16bit_11k 2',
                        waveformNames: ['16bit_11k 2'],
                    }),
                ],
                { kind: 'SAMPLES' },
            ),
        );
        expect(imageTransport.releaseClientUpload).not.toHaveBeenCalled();
    });

    it('waits for the complete batch before naming or validating inspected rows', async () => {
        const inspections = new Map<string, (value: AudioSourceInfo) => void>();
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn(
            (source) =>
                new Promise<AudioSourceInfo>((resolve) => {
                    inspections.set(source.displayName, resolve);
                }),
        );
        const first = serverFileLocation({ rootId: 'workspace', relativePath: 'First.wav' }, 'First.wav');
        const second = serverFileLocation({ rootId: 'workspace', relativePath: 'Second.wav' }, 'Second.wav');
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [first, second],
                target: { partitionIndex: 0, volumeName: 'Batch' },
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await waitFor(() => expect(imageTransport.inspectAudio).toHaveBeenCalledTimes(2));
        expect(screen.getByRole('progressbar', { name: 'Inspecting audio files' })).toBeTruthy();
        expect(screen.getByText('Inspecting 0 of 2 files')).toBeTruthy();

        inspections.get('First.wav')!(
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                sourceSampleWidthBits: 16,
                sampleWidthConverted: false,
            }),
        );
        await waitFor(() => expect(screen.getByText('Inspecting 1 of 2 files')).toBeTruthy());
        expect(screen.queryByText('Sample names must be 1-16 printable ASCII characters.')).toBeNull();
        expect(screen.queryByLabelText('Sample name for First.wav')).toBeNull();
        expect((screen.getByRole('button', { name: 'Import 2 files' }) as HTMLButtonElement).disabled).toBe(true);

        inspections.get('Second.wav')!(
            sourceInfo({
                sourceFormat: 'WAV',
                sourceSubtype: 'PCM_16',
                channels: 1,
                sourceSampleWidthBits: 16,
                sampleWidthConverted: false,
            }),
        );
        expect(((await screen.findByLabelText('Sample name for First.wav')) as HTMLInputElement).value).toBe('First');
        expect((screen.getByLabelText('Sample name for Second.wav') as HTMLInputElement).value).toBe('Second');
        expect(screen.queryByRole('progressbar', { name: 'Inspecting audio files' })).toBeNull();
        expect(screen.queryByText('Sample names must be 1-16 printable ASCII characters.')).toBeNull();
    });

    it('renders one responsive card per mono or stereo file', async () => {
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
        const monoCard = screen.getByRole('group', { name: `Audio import file ${monoFilename}` });
        expect(within(monoCard).getByRole('button', { name: `Play ${monoFilename}` })).toBeTruthy();
        expect(within(monoCard).getByLabelText(`Wave data (mono/left) for ${monoFilename}`)).toBeTruthy();
        expect(within(monoCard).queryByLabelText(`Wave data (right) for ${monoFilename}`)).toBeNull();
        expect(within(monoCard).getAllByLabelText(`Root key for ${monoFilename}`)).toHaveLength(1);
        const monoDetailsButton = within(monoCard).getByRole('button', {
            name: `Import details for ${monoFilename}`,
        });
        expect(monoDetailsButton.classList.contains('has-adjustments')).toBe(false);

        const stereoCard = screen.getByRole('group', { name: 'Audio import file Stereo pad.wav' });
        expect(within(stereoCard).getByLabelText('Wave data (mono/left) for Stereo pad.wav')).toBeTruthy();
        expect(within(stereoCard).getByLabelText('Wave data (right) for Stereo pad.wav')).toBeTruthy();
        expect(within(stereoCard).getAllByLabelText('Root key for Stereo pad.wav')).toHaveLength(1);
        expect(screen.queryByRole('button', { name: 'Settings' })).toBeNull();

        await fireEvent.click(monoDetailsButton);
        expect(within(monoCard).getByText('Initial value sources')).toBeTruthy();
        expect(within(monoCard).getAllByText('A-series default (no supported WAV value was applied)')).toHaveLength(3);

        await fireEvent.click(within(stereoCard).getByRole('button', { name: 'Import details for Stereo pad.wav' }));
        expect(within(monoCard).queryByText('Initial value sources')).toBeNull();
        expect(within(stereoCard).getByText('Initial value sources')).toBeTruthy();
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

        await fireEvent.input(screen.getByLabelText('Root key for Stereo piano.flac'), {
            target: { value: '69' },
        });
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));

        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [
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
                ],
                { kind: 'SAMPLES' },
            ),
        );
        expect(imageTransport.releaseClientUpload).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { uploadId: 'audio-stereo' } }),
        );
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('shows neutral progress instead of self-conflicts while a committed import refreshes the catalog', async () => {
        let finishCommit!: () => void;
        const commit = new Promise<void>((resolve) => {
            finishCommit = resolve;
        });
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(sourceInfo({ channels: 1 }));
        const oncommit = vi.fn(() => commit);
        const oncancel = vi.fn();
        const file = serverFileLocation({ rootId: 'workspace', relativePath: 'Fresh.wav' }, 'Fresh.wav');
        const baseProps = {
            transport: imageTransport,
            files: [file],
            target: { partitionIndex: 0, volumeName: 'Import' },
            existingSampleNames: [] as string[],
            existingWaveformNames: [] as string[],
            oncommit,
            oncancel,
        };
        const rendered = render(AudioImportDialog, { props: baseProps });

        expect(await screen.findAllByDisplayValue('Fresh')).toHaveLength(2);
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() => expect(oncommit).toHaveBeenCalledOnce());
        await rendered.rerender({
            ...baseProps,
            existingSampleNames: ['Fresh'],
            existingWaveformNames: ['Fresh'],
        });

        expect(screen.getByText('Importing…')).toBeTruthy();
        expect(screen.queryByText('Sample name already exists: Fresh')).toBeNull();
        expect(screen.queryByText('Wave data name already exists: Fresh')).toBeNull();
        expect(screen.queryByText(/^Fits/)).toBeNull();
        expect(screen.queryByRole('button', { name: 'Import details for Fresh.wav' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Remove Fresh.wav' })).toBeNull();
        expect((screen.getByRole('button', { name: 'Importing' }) as HTMLButtonElement).disabled).toBe(true);

        finishCommit();
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('restores current validation after a commit fails', async () => {
        let failCommit!: (error: Error) => void;
        const commit = new Promise<void>((_resolve, reject) => {
            failCommit = reject;
        });
        const file = serverFileLocation({ rootId: 'workspace', relativePath: 'Retry.wav' }, 'Retry.wav');
        const oncommit = vi.fn(() => commit);
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(sourceInfo({ channels: 1 }));
        const baseProps = {
            transport: imageTransport,
            files: [file],
            target: { partitionIndex: 0, volumeName: 'Import' },
            existingSampleNames: [] as string[],
            existingWaveformNames: [] as string[],
            oncommit,
            oncancel: vi.fn(),
        };
        render(AudioImportDialog, { props: baseProps });

        expect(await screen.findAllByDisplayValue('Retry')).toHaveLength(2);
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() => expect(oncommit).toHaveBeenCalledOnce());
        expect(screen.getByText('Importing…')).toBeTruthy();

        failCommit(new Error('Import transaction failed'));
        expect(await screen.findByText('Import transaction failed')).toBeTruthy();
        expect(screen.queryByText('Importing…')).toBeNull();
        expect(screen.getByText(/^Fits/)).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Import details for Retry.wav' })).toBeTruthy();
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
                    loopStartFrame: 70_000,
                    loopLengthFrames: 10_000,
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

        const card = await screen.findByRole('group', { name: 'Audio import file Mapped.wav' });
        expect(await within(card).findByLabelText('Sampler settings for Mapped.wav')).toBeTruthy();
        expect(screen.getByDisplayValue('-63')).toBeTruthy();
        expect(screen.getByDisplayValue('70000')).toBeTruthy();
        const playbackMode = screen.getByRole('combobox', {
            name: 'Playback mode for Mapped.wav',
        }) as HTMLSelectElement;
        expect(playbackMode.value).toBe('1');
        const forwardLoopOption = within(playbackMode).getByRole('option', {
            name: 'Forward loop',
        }) as HTMLOptionElement;
        expect(forwardLoopOption.selected).toBe(true);
        expect(within(card).queryByText('Initial value sources')).toBeNull();
        expect(within(card).queryByText('An additional WAV sampler loop was ignored.')).toBeNull();

        const detailsButton = within(card).getByRole('button', { name: 'Import details for Mapped.wav' });
        expect(detailsButton.getAttribute('aria-expanded')).toBe('false');
        expect(detailsButton.classList.contains('has-adjustments')).toBe(true);
        await fireEvent.click(detailsButton);

        expect(detailsButton.getAttribute('aria-expanded')).toBe('true');
        expect(within(card).getByText('Initial value sources')).toBeTruthy();
        expect(within(card).getByText('Pitch (root key and fine tune)')).toBeTruthy();
        expect(within(card).getByText('Key and velocity ranges')).toBeTruthy();
        expect(within(card).getByText('Playback and loop')).toBeTruthy();
        expect(within(card).getAllByText('WAV sampler metadata (smpl chunk)')).toHaveLength(2);
        expect(within(card).getByText('WAV instrument metadata (inst chunk)')).toBeTruthy();
        expect(within(card).getByText('Import adjustments')).toBeTruthy();
        expect(within(card).getAllByText('An additional WAV sampler loop was ignored.')).toHaveLength(2);

        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [
                    expect.objectContaining({
                        rootKey: 62,
                        fineTuneCents: -63,
                        keyLow: 12,
                        keyHigh: 96,
                        velocityLow: 8,
                        velocityHigh: 110,
                        loopMode: 1,
                        loopStartFrame: 70_000,
                        loopLengthFrames: 10_000,
                    }),
                ],
                { kind: 'SAMPLES' },
            ),
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
        expect(selector.classList).toContain('dialog-field-control');
        expect((selector as HTMLSelectElement).value).toBe('44100');

        await fireEvent.change(selector, { target: { value: '22050' } });
        await waitFor(() => expect(imageTransport.inspectAudio).toHaveBeenLastCalledWith(expect.anything(), 22_050));
        await waitFor(() => expect((selector as HTMLSelectElement).value).toBe('22050'));
        expect(screen.getByText('WAV PCM_16 · Mono · 96,000 Hz · 16-bit · resampled TPDF · 1.00 s')).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [
                    expect.objectContaining({
                        targetSampleRate: 22_050,
                    }),
                ],
                { kind: 'SAMPLES' },
            ),
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
