<script lang="ts">
    import AudioImportDialog from '../lib/components/AudioImportDialog.svelte';
    import { ImportCompletion } from '../features/import/importCompletion.svelte';
    import { JobController } from '../features/jobs/actions';
    import { serverFileLocation } from '../lib/storageLocations';
    import type { ImageTransport } from '../lib/transport';

    let open = $state(true);
    let names = $state<string[]>([]);
    const warning = new URLSearchParams(location.search).has('warning');
    const transport = {
        audioImportCapabilities: async () => ({
            supportedSampleRates: [44100],
            defaultUnsupportedSampleRate: 44100,
            supportedOutputSampleWidthsBits: [16],
            sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8',
            maximumUploads: 1024,
        }),
        inspectAudio: async () => ({
            sourceFormat: 'WAV',
            sourceSubtype: 'PCM_24',
            channels: 1,
            frameCount: 44100,
            sourceSampleRate: 44100,
            outputSampleRate: 44100,
            sourceSampleWidthBits: 24,
            outputSampleWidthBits: 16,
            durationSeconds: 1,
            resampled: false,
            quantized: true,
            sampleWidthConverted: true,
            ditherAlgorithm: 'axk-tpdf-pcg32-v1',
            projectedOutputFrameCount: 44100,
            projectedOutputBytesPerChannel: 88200,
            projectedOutputBytesTotal: 88200,
            maximumOutputFrameCountPerChannel: 1 << 24,
            maximumOutputBytesPerChannel: 32 * 1024 * 1024,
            valid: true,
            issues: [],
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
        }),
        waitForJob: async () => ({
            jobId: 1,
            status: 'completed',
            result: {
                kind: 'ALTERATION',
                operations: [],
                warnings: warning ? [{ message: 'New conversion warning' }] : [],
            },
        }),
    } as unknown as ImageTransport;
    const completion = new ImportCompletion(transport, new JobController(transport));
</script>

{#if open}
    <AudioImportDialog
        {transport}
        {completion}
        files={[serverFileLocation({ rootId: 'test', relativePath: 'Fresh.wav' }, 'Fresh.wav')]}
        target={{ kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' }}
        destinationMode="existing"
        destinationPartitionIndex={0}
        destinationVolumeName="Imported"
        partitionOptions={[{ partitionIndex: 0, name: 'Partition 1' }]}
        volumeOptions={[{ partitionIndex: 0, name: 'Partition 1', volumeName: 'Imported', label: 'Imported' }]}
        existingSampleNames={names}
        existingWaveformNames={names}
        ondestinationmode={() => {}}
        ondestinationvolume={() => {}}
        ondestinationpartition={() => {}}
        ondestinationname={() => {}}
        oncommit={() =>
            completion.run(
                async () => ({ jobId: 1, status: 'queued', kind: 'alter' }),
                async () => {
                    names = ['Fresh'];
                },
            )}
        oncancel={() => (open = false)}
    />
{:else}
    <p>Import closed</p>
{/if}
