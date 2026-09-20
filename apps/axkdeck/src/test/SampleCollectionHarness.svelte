<script lang="ts">
    import { untrack } from 'svelte';
    import SampleCollection from '../features/backends/axklib/SampleCollection.svelte';
    import { provideObjectEditors } from '../features/object-editor/context';
    import type { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import type { PackageExportSelectionState } from '../lib/objectSelection';
    import type { SampleStructureItem } from '../lib/types';

    let {
        workflow,
        samples,
        initiallySelected = ['A', 'B'],
        initiallyActive = 'A',
        mutationsAvailable = true,
        view = 'samples',
    }: {
        workflow: ObjectEditorWorkflow;
        samples: SampleStructureItem[];
        initiallySelected?: string[];
        initiallyActive?: string;
        mutationsAvailable?: boolean;
        view?: 'samples' | 'sample-banks';
    } = $props();
    provideObjectEditors(untrack(() => workflow));
    let selection = $state<PackageExportSelectionState>(
        untrack(() => ({
            items: samples
                .filter((sample) => initiallySelected.includes(sample.objectId))
                .map((sample) => ({
                    kind: 'SBNK',
                    objectId: sample.objectId,
                    name: sample.name,
                    typeLabel: 'Sample',
                    partitionIndex: sample.object.partitionIndex,
                    partitionName: sample.object.partitionName,
                    volumeName: sample.object.volumeName,
                })),
            anchors: {},
        })),
    );
    let activeSampleId = $state(untrack(() => initiallyActive));
    let lowerOpen = $state(false);
    let queries = $state({ primary: '', secondary: '', tertiary: '' });
</script>

<output aria-label="Active Sample">{activeSampleId}</output>
<output aria-label="Selected Samples">{selection.items.map((item) => item.objectId).join(',')}</output>
<output aria-label="Lower zone open">{String(lowerOpen)}</output>
<SampleCollection
    sessionId={1}
    revision={1}
    bind:lowerOpen
    {view}
    {samples}
    sampleBanks={[]}
    waveData={[]}
    activeSampleBankId=""
    {activeSampleId}
    activeWaveDataId=""
    {queries}
    onquerychange={(lane, value) => (queries[lane] = value)}
    onsamplebankselect={() => {}}
    onsampleselect={(sample) => (activeSampleId = sample.objectId)}
    onwavedataselect={() => {}}
    auditionableSampleIds={new Set()}
    auditionableSampleBankIds={new Set()}
    objectRenameAvailable={mutationsAvailable}
    packageExportAvailable={true}
    {selection}
    onselectionchange={(next) => (selection = next)}
/>
