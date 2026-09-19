<script lang="ts">
    import ContainedObjectWorkspace from '../lib/components/ContainedObjectWorkspace.svelte';
    import type { SampleStructureItem } from '../lib/types';
    let { selected, stereo, onselect }: { selected: string; stereo: boolean; onselect: (id: string) => void } =
        $props();
    const samples: SampleStructureItem[] = [
        'Sample A',
        'Sample B',
        'Sample C with a very long name that must truncate in narrow panes',
    ].map((name) => ({
        id: name,
        objectId: name,
        name,
        objectType: 'SBNK',
        membershipLabel: 'Standalone',
        object: {
            key: name,
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
    }));
    let queries = $state({ primary: '', secondary: '', tertiary: '' });
</script>

<ContainedObjectWorkspace
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
    selection={{
        items: [
            {
                kind: 'SBNK',
                objectId: selected,
                name: selected,
                typeLabel: 'Sample',
                partitionIndex: 0,
                partitionName: 'Test',
                volumeName: 'Test',
            },
        ],
        anchors: {},
    }}
/>
