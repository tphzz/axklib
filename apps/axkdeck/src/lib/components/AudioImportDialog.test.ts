import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import { clientUploadLocation, serverFileLocation } from '../storageLocations';
import type { ClientUploadSource } from '../clientUploadSource';
import type { AudioImportOptions } from '../audioImportOptions';
import type { AudioImportCapabilities, AudioSourceInfo, ImageTransport } from '../transport';
import AudioImportDialog from './AudioImportDialog.svelte';
import { ImportCompletion } from '../../features/import/importCompletion.svelte';
import { JobController } from '../../features/jobs/actions';

const audioImportDialogSource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/AudioImportDialog.svelte'),
    'utf8',
);
const audioImportRowsSource = readFileSync(resolve(process.cwd(), 'src/lib/components/AudioImportRows.svelte'), 'utf8');
const targetSettingsSource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/AudioImportTargetSettings.svelte'),
    'utf8',
);
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

function destinationProps(volumeName: string, partitionIndex = 0) {
    const partitionName = `Partition ${partitionIndex + 1}`;
    return {
        completion: new ImportCompletion(transport(), new JobController(transport())),
        target: { kind: 'EXISTING_VOLUME' as const, partitionIndex, volumeName },
        destinationMode: 'existing' as const,
        destinationPartitionIndex: partitionIndex,
        destinationVolumeName: volumeName,
        partitionOptions: [{ partitionIndex, name: partitionName }],
        volumeOptions: [
            {
                partitionIndex,
                name: partitionName,
                volumeName,
                label: `${partitionName} / ${volumeName}`,
            },
        ],
        ondestinationmode: vi.fn(),
        ondestinationvolume: vi.fn(),
        ondestinationpartition: vi.fn(),
        ondestinationname: vi.fn(),
    };
}

describe('AudioImportDialog', () => {
    it('chooses one format for the batch without reinspecting audio or changing row settings', async () => {
        const imageTransport = transport();
        const oncommit = vi.fn().mockResolvedValue(false);
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [serverFileLocation({ rootId: 'workspace', relativePath: 'Tone.wav' }, 'Tone.wav')],
                ...destinationProps('Import'),
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });
        await screen.findByDisplayValue('Tone');
        const formats = screen.getByRole('group', { name: 'Sample format' });
        expect(within(formats).getByRole('button', { name: 'a3k' }).getAttribute('aria-pressed')).toBe('true');
        await fireEvent.click(within(formats).getByRole('button', { name: 'a4k/a5k' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        expect(oncommit).toHaveBeenCalledWith(
            [expect.objectContaining({ sampleName: 'Tone', rootKey: 60 })],
            { sampleFormat: 'A4000_A5000_224', grouping: { kind: 'SAMPLES' } },
            expect.any(Array),
        );
        expect(imageTransport.inspectAudio).toHaveBeenCalledOnce();
        expect(screen.getAllByRole('group', { name: 'Sample format' })).toHaveLength(1);
    });

    it('locks the batch format through submission and restores editing after a confirmed failure', async () => {
        let finish!: (result: boolean) => void;
        const oncommit = vi.fn(
            (_rows: unknown, _options: AudioImportOptions) => new Promise<boolean>((resolve) => (finish = resolve)),
        );
        render(AudioImportDialog, {
            props: {
                transport: transport(),
                files: [serverFileLocation({ rootId: 'workspace', relativePath: 'Tone.wav' }, 'Tone.wav')],
                ...destinationProps('Import'),
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });
        await screen.findByDisplayValue('Tone');
        await waitFor(() =>
            expect(screen.getByRole<HTMLButtonElement>('button', { name: 'Import' }).disabled).toBe(false),
        );
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        const group = screen.getByRole('group', { name: 'Sample format' });
        for (const button of within(group).getAllByRole<HTMLButtonElement>('button'))
            expect(button.disabled).toBe(true);
        expect(oncommit.mock.calls[0]?.[1]).toEqual({ sampleFormat: 'A3000_188', grouping: { kind: 'SAMPLES' } });
        finish(false);
        await waitFor(() =>
            expect(within(group).getByRole<HTMLButtonElement>('button', { name: 'a4k/a5k' }).disabled).toBe(false),
        );
    });

    it('does not revalidate imported names after refresh when new warnings retain the dialog', async () => {
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(sourceInfo({ channels: 1 }));
        imageTransport.waitForJob = vi.fn().mockResolvedValue({
            jobId: 1,
            status: 'completed',
            result: { kind: 'ALTERATION', warnings: [{ message: 'Conversion warning' }], operations: [] },
        });
        const completion = new ImportCompletion(imageTransport, new JobController(imageTransport));
        const oncancel = vi.fn();
        const start = vi.fn().mockResolvedValue({ jobId: 1, kind: 'alter', status: 'queued' });
        const props = {
            transport: imageTransport,
            files: [serverFileLocation({ rootId: 'workspace', relativePath: 'Fresh.wav' }, 'Fresh.wav')],
            ...destinationProps('Import'),
            completion,
            existingSampleNames: [] as string[],
            existingWaveformNames: [] as string[],
            existingSampleBankNames: [] as string[],
            oncommit: vi.fn(() =>
                completion.run(start, async () => {
                    await rendered.rerender({
                        ...props,
                        existingSampleNames: ['Fresh'],
                        existingWaveformNames: ['Fresh'],
                        existingSampleBankNames: ['New Bank'],
                    });
                }),
            ),
            oncancel,
        };
        const rendered = render(AudioImportDialog, { props });
        await screen.findAllByDisplayValue('Fresh');
        await fireEvent.change(screen.getByLabelText('Import mode'), { target: { value: 'SAMPLE_BANK' } });
        await fireEvent.input(screen.getByLabelText('Sample Bank name'), { target: { value: 'New Bank' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        await screen.findByRole('button', { name: 'Done' });
        expect(screen.queryByText(/name already exists/)).toBeNull();
        expect(screen.getByText('Imported', { exact: true })).toBeTruthy();
        expect(screen.queryByText(/^Fits/)).toBeNull();
        expect(screen.queryByRole('button', { name: 'Remove Fresh.wav' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Import' })).toBeNull();
        expect(oncancel).not.toHaveBeenCalled();
        await waitFor(() =>
            expect((screen.getByRole('button', { name: 'Done' }) as HTMLButtonElement).disabled).toBe(false),
        );
        await fireEvent.click(screen.getByRole('button', { name: 'Done' }));
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(start).toHaveBeenCalledOnce();
    });

    it('retains completion warnings with Done and never offers another import', async () => {
        const imageTransport = transport();
        imageTransport.waitForJob = vi.fn().mockResolvedValue({
            jobId: 1,
            status: 'completed',
            result: { kind: 'ALTERATION', warnings: [{ message: 'Audio conversion clipped' }], operations: [] },
        });
        const completion = new ImportCompletion(imageTransport, new JobController(imageTransport));
        await completion.run(async () => ({ jobId: 1, kind: 'alter', status: 'queued' }), vi.fn());
        const oncancel = vi.fn();
        const oncommit = vi.fn();
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [],
                ...destinationProps('Import'),
                completion,
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit,
                oncancel,
            },
        });
        expect(screen.getByRole('region', { name: 'Import warnings' }).textContent).toContain(
            'Audio conversion clipped',
        );
        expect(screen.queryByRole('button', { name: 'Import' })).toBeNull();
        expect(oncancel).not.toHaveBeenCalled();
        await fireEvent.click(screen.getByRole('button', { name: 'Done' }));
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(oncommit).not.toHaveBeenCalled();
    });
    it('keeps a committed import open and only refreshes when Refresh is clicked', async () => {
        const imageTransport = transport();
        imageTransport.waitForJob = vi.fn().mockResolvedValue({ jobId: 1, status: 'completed' });
        const completion = new ImportCompletion(imageTransport, new JobController(imageTransport));
        let finishRefresh!: () => void;
        const refresh = vi
            .fn()
            .mockRejectedValueOnce(new Error('Offline'))
            .mockImplementation(
                () =>
                    new Promise<void>((resolve) => {
                        finishRefresh = resolve;
                    }),
            );
        const start = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        await completion.run(start, refresh);
        const oncancel = vi.fn();
        const { container } = render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [serverFileLocation({ rootId: 'workspace', relativePath: 'audio.wav' })],
                ...destinationProps('Import'),
                completion,
                existingSampleNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel,
            },
        });
        expect(screen.queryByRole('button', { name: 'Import' })).toBeNull();
        const footer = container.querySelector('.dialog-footer')!;
        const buttons = within(footer as HTMLElement).getAllByRole('button') as HTMLButtonElement[];
        expect(buttons.map((button) => button.textContent?.trim())).toEqual(['Close', 'Refresh']);
        expect(buttons[0].disabled).toBe(false);
        expect(footer.querySelector('.dialog-footer-status')).toBeTruthy();
        const style = document.createElement('style');
        style.textContent =
            readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8').match(
                /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
            )?.[0] ?? '';
        document.head.append(style);
        for (const button of buttons) {
            expect(getComputedStyle(button).height).toBe('30px');
            expect(getComputedStyle(button).marginTop).toBe('0px');
            expect(getComputedStyle(button).marginBottom).toBe('0px');
        }
        style.remove();
        expect(oncancel).not.toHaveBeenCalled();
        await fireEvent.click(buttons[1]);
        await waitFor(() => expect(completion.phase).toBe('refreshing'));
        expect((screen.getByRole('button', { name: 'Refresh' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.queryByRole('button', { name: 'Import' })).toBeNull();
        finishRefresh();
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(start).toHaveBeenCalledTimes(1);
        expect(refresh).toHaveBeenCalledTimes(2);
    });

    it('owns the compact spacing around the reusable destination chooser', () => {
        expect(audioImportDialogSource).toMatch(/\.audio-import-body\s*\{[^}]*gap:\s*10px;/s);
    });

    it('keeps batch format and grouping in compact responsive shared controls', async () => {
        render(AudioImportDialog, {
            props: {
                transport: transport(),
                files: [new File([new Uint8Array(64)], 'Bass.wav', { type: 'audio/wav' })],
                ...destinationProps('Sounds'),
                existingSampleNames: [],
                existingSampleBankNames: [],
                existingWaveformNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const mode = screen.getByRole('combobox', { name: 'Import mode' });
        expect(mode.classList).toContain('dialog-field-control');
        await fireEvent.change(mode, { target: { value: 'SAMPLE_BANK' } });
        expect(screen.getByRole('textbox', { name: 'Sample Bank name' }).classList).toContain('dialog-field-control');
        expect(targetSettingsSource).toMatch(
            /\.import-target-settings\s*\{[^}]*grid-template-columns:\s*max-content minmax\(0, 360px\) max-content max-content;[^}]*align-items:\s*center;/s,
        );
        expect(targetSettingsSource).toMatch(
            /@media \(max-width: 760px\)\s*\{[^}]*\.import-target-settings\s*\{[^}]*grid-template-columns:\s*max-content minmax\(0, 1fr\);/s,
        );
        expect(screen.getByRole('group', { name: 'Sample format' }).classList).toContain('dialog-segmented-control');
    });

    it('left-packs sampler fields independently from the identity columns', () => {
        expect(audioSamplerSettingsSource).toMatch(
            /\.settings-fields\s*\{[^}]*display:\s*flex;[^}]*flex-wrap:\s*wrap;[^}]*justify-content:\s*flex-start;/s,
        );
        expect(audioSamplerSettingsSource).toMatch(/\.settings-fields\s*>\s*label\s*\{[^}]*flex:\s*0 0 auto;/s);
        expect(audioSamplerSettingsSource).not.toContain('grid-template-columns: repeat(7');
        expect(audioImportRowsSource).toContain('padding-right: 12px;');
        expect(audioImportRowsSource).toContain('scrollbar-gutter: stable;');
    });

    it('keeps each audio file compact while allowing its source details to wrap', () => {
        expect(audioImportRowsSource).toContain('<small class="audio-import-file-metadata">');
        expect(audioImportRowsSource).toMatch(
            /\.audio-import-file-heading\s*\{[^}]*display:\s*flex;[^}]*flex-wrap:\s*wrap;/,
        );
        expect(audioImportRowsSource).toMatch(/\.audio-import-rows\s*\{[^}]*gap:\s*6px;/);
        expect(audioImportRowsSource).toMatch(/\.audio-import-card\s*\{[^}]*gap:\s*6px;[^}]*padding:\s*8px 10px;/);
        expect(audioImportRowsSource).toMatch(/\.identity-fields\s*\{[^}]*gap:\s*6px 10px;/);
        expect(audioSamplerSettingsSource).toMatch(/\.settings-fields\s*\{[^}]*gap:\s*6px 10px;/);
    });

    it('imports inspected Samples into a newly named Sample Bank', async () => {
        const oncommit = vi.fn().mockResolvedValue(true);
        render(AudioImportDialog, {
            props: {
                transport: transport(),
                files: [new File([new Uint8Array(64)], 'Bass.wav', { type: 'audio/wav' })],
                ...destinationProps('Sounds'),
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
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);

        await fireEvent.input(name, { target: { value: 'Bass Bank' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [expect.objectContaining({ sampleName: 'Bass' })],
                {
                    sampleFormat: 'A3000_188',
                    grouping: { kind: 'SAMPLE_BANK', sampleBankName: 'Bass Bank' },
                },
                [],
            ),
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
                ...destinationProps('Bulk'),
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
                ...destinationProps('My Volume'),
                existingSampleNames: [],
                existingWaveformNames: [],
                onchooseworkspace,
                onchooselocal,
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('heading', { name: 'Choose audio files' })).toBeTruthy();
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
        const oncommit = vi.fn().mockResolvedValue(true);
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'audio/16bit_11k.wav' },
            'Yamaha/audio/16bit_11k.wav',
        );
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [workspaceFile],
                ...destinationProps('My Volume'),
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

        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [
                    expect.objectContaining({
                        source: workspaceFile,
                        sampleName: '16bit_11k 2',
                        waveformNames: ['16bit_11k 2'],
                    }),
                ],
                { sampleFormat: 'A3000_188', grouping: { kind: 'SAMPLES' } },
                [],
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
                ...destinationProps('Batch'),
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
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);

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
                ...destinationProps('Mixed'),
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
        const oncommit = vi.fn().mockResolvedValue(true);
        const oncancel = vi.fn();
        const file = new File([new Uint8Array(512)], 'Stereo piano.flac', { type: 'audio/flac' });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [file],
                ...destinationProps('Keys', 2),
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
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));

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
                { sampleFormat: 'A3000_188', grouping: { kind: 'SAMPLES' } },
                [],
            ),
        );
        expect(imageTransport.releaseClientUpload).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { uploadId: 'audio-stereo' } }),
        );
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('shows neutral progress instead of self-conflicts while a committed import refreshes the catalog', async () => {
        let finishCommit!: (success: boolean) => void;
        const commit = new Promise<boolean>((resolve) => {
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
            ...destinationProps('Import'),
            existingSampleNames: [] as string[],
            existingWaveformNames: [] as string[],
            oncommit,
            oncancel,
        };
        const rendered = render(AudioImportDialog, { props: baseProps });

        expect(await screen.findAllByDisplayValue('Fresh')).toHaveLength(2);
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
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
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);

        finishCommit(true);
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('restores current validation after a commit fails', async () => {
        let failCommit!: (error: Error) => void;
        const commit = new Promise<boolean>((_resolve, reject) => {
            failCommit = reject;
        });
        const file = serverFileLocation({ rootId: 'workspace', relativePath: 'Retry.wav' }, 'Retry.wav');
        const oncommit = vi.fn(() => commit);
        const imageTransport = transport();
        imageTransport.inspectAudio = vi.fn().mockResolvedValue(sourceInfo({ channels: 1 }));
        const baseProps = {
            transport: imageTransport,
            files: [file],
            ...destinationProps('Import'),
            existingSampleNames: [] as string[],
            existingWaveformNames: [] as string[],
            oncommit,
            oncancel: vi.fn(),
        };
        render(AudioImportDialog, { props: baseProps });

        expect(await screen.findAllByDisplayValue('Retry')).toHaveLength(2);
        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        await waitFor(() => expect(oncommit).toHaveBeenCalledOnce());
        expect(screen.getByText('Importing…')).toBeTruthy();

        failCommit(new Error('Import transaction failed'));
        expect(await screen.findAllByText('Import transaction failed')).toHaveLength(2);
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
        const oncommit = vi.fn().mockResolvedValue(true);
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [new File([new Uint8Array(128)], 'Mapped.wav', { type: 'audio/wav' })],
                ...destinationProps('Mapped'),
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

        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
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
                { sampleFormat: 'A3000_188', grouping: { kind: 'SAMPLES' } },
                ['An additional WAV sampler loop was ignored.', 'An additional WAV sampler loop was ignored.'],
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
        const oncommit = vi.fn().mockResolvedValue(true);
        const file = new File([new Uint8Array(128)], 'Unsupported rate.wav', { type: 'audio/wav' });
        render(AudioImportDialog, {
            props: {
                transport: imageTransport,
                files: [file],
                ...destinationProps('Rates'),
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

        await fireEvent.click(screen.getByRole('button', { name: 'Import' }));
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith(
                [
                    expect.objectContaining({
                        targetSampleRate: 22_050,
                    }),
                ],
                { sampleFormat: 'A3000_188', grouping: { kind: 'SAMPLES' } },
                [],
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
                ...destinationProps('Voice'),
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
                ...destinationProps('Mixed'),
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
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(false);
    });
});
