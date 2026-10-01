<script lang="ts">
    import SampleCollection from '../features/backends/axklib/SampleCollection.svelte';
    import ObjectRenameDialog from '../lib/components/ObjectRenameDialog.svelte';
    import type { ObjectRenameTarget, SampleStructureItem } from '../lib/types';
    import type { PackageExportSelectionState } from '../lib/objectSelection';
    import type { SampleStorageFormat } from '../lib/objectEditing';
    import { sampleFormatFixture } from './sampleFormatFixture';
    let {
        selected,
        stereo,
        onselect,
        names = ['Sample A', 'Sample B', 'Sample C with a very long name that must truncate in narrow panes'],
        revision = 1,
        storedFormats = {},
    }: {
        selected: string;
        stereo: boolean;
        onselect: (id: string) => void;
        names?: string[];
        revision?: number;
        storedFormats?: Record<string, SampleStorageFormat>;
    } = $props();
    let rename = $state<ObjectRenameTarget | null>(null);
    let selection = $state<PackageExportSelectionState>({ items: [], anchors: {} });
    const samples: SampleStructureItem[] = $derived(
        names.map((name) => ({
            id: name,
            objectId: name,
            name,
            objectType: 'SBNK',
            membershipLabel: 'Standalone',
            object: {
                key: name,
                sampleFormat: sampleFormatFixture(storedFormats[name] ?? (stereo ? 'A3000_188' : 'A4000_A5000_224'))
                    .sampleFormat,
                name,
                objectType: 'SBNK',
                partitionIndex: 0,
                partitionName: 'Test',
                volumeName: 'Test',
                categoryName: 'Samples',
                objectEncoding: 'current',
                directoryEntryName: name,
                sfsId: 0,
                storedSizeBytes: 356,
                sizeWithDependenciesBytes: 610304,
                sampleRate: 44100,
                rootKey: 60,
                storedFrameCount: 84000,
                waveStartFrame: 0,
                waveLengthFrames: 84000,
                storageState: 'COMPLETE',
                sampleWidthBytes: 2,
            },
        })),
    );
    let queries = $state({ primary: '', secondary: '', tertiary: '' });
</script>

<SampleCollection
    sessionId={1}
    {revision}
    view="samples"
    sampleBanks={[]}
    {samples}
    waveData={[]}
    activeSampleBankId=""
    activeSampleId={selected}
    activeWaveDataId=""
    {queries}
    onquerychange={(lane, value) => (queries[lane] = value)}
    onsamplebankselect={() => {}}
    onsampleselect={(item) => onselect(item.objectId)}
    onwavedataselect={() => {}}
    auditionableSampleIds={new Set()}
    auditionableSampleBankIds={new Set()}
    stereoSampleIds={stereo ? new Set(samples.map((item) => item.objectId)) : new Set()}
    {selection}
    onselectionchange={(value) => (selection = value)}
    objectRenameAvailable
    onrenameobject={(target) => (rename = target)}
/>
{#if rename}<ObjectRenameDialog
        target={rename}
        busy={false}
        error=""
        oncancel={() => (rename = null)}
        onsubmit={() => (rename = null)}
    />{/if}
