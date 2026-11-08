<script lang="ts">
    import AudioImportDialog from '../lib/components/AudioImportDialog.svelte';
    import { createAudioImportLayoutFixture } from './audioImportLayoutFixture';
    import type { ImportDestinationMode } from '../features/import/packageDestinations';

    const params = new URLSearchParams(location.search);
    const scenario =
        params.has('native-cases') && !params.has('short-stereo') ? 'mono' : (params.get('scenario') ?? 'normal');
    const fixture = createAudioImportLayoutFixture(scenario);
    Object.assign(window, { audioImportLayoutFixture: fixture });
    let open = $state(true);
    let mode = $state<ImportDestinationMode>('existing');
    let partitionIndex = $state(0);
    let volumeName = $state('A3K_FLOPPY_03');
    const volumeOptions = [
        { partitionIndex: 0, name: 'internet', volumeName: 'A3K_FLOPPY_03', label: 'A3K_FLOPPY_03' },
        { partitionIndex: 0, name: 'internet', volumeName: 'Another volume', label: 'Another volume' },
    ];
</script>

{#if open}
    <AudioImportDialog
        transport={fixture.transport}
        completion={fixture.completion}
        files={scenario === 'empty' ? [] : fixture.files}
        target={mode === 'existing'
            ? { kind: 'EXISTING_VOLUME', partitionIndex, volumeName }
            : { kind: 'CREATE_VOLUME', partitionIndex, volumeName }}
        destinationMode={mode}
        destinationPartitionIndex={partitionIndex}
        destinationVolumeName={volumeName}
        partitionOptions={[{ partitionIndex: 0, name: 'internet' }]}
        {volumeOptions}
        existingSampleNames={fixture.existingSampleNames}
        existingWaveformNames={[]}
        ondestinationmode={(next) => (mode = next)}
        ondestinationvolume={(index, name) => {
            partitionIndex = index ?? 0;
            volumeName = name;
        }}
        ondestinationpartition={(index) => (partitionIndex = index)}
        ondestinationname={(name) => (volumeName = name)}
        sampleFormat="A4000_A5000_224"
        oncommit={fixture.commit}
        oncancel={() => (open = false)}
    />
{:else}
    <p>Import closed</p>
{/if}
