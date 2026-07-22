<script lang="ts">
    import type { SampleStructureItem, WaveDataItem } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';

    type ContainedView = 'sample-banks' | 'samples';
    type LaneId = 'primary' | 'secondary' | 'tertiary';

    interface LaneQueries {
        primary: string;
        secondary: string;
        tertiary: string;
    }

    interface Props {
        view: ContainedView;
        sampleBanks: SampleStructureItem[];
        samples: SampleStructureItem[];
        waveData: WaveDataItem[];
        activeSampleBankId: string;
        activeSampleId: string;
        activeWaveDataId: string;
        queries: LaneQueries;
        onquerychange: (lane: LaneId, value: string) => void;
        onsamplebankselect: (item: SampleStructureItem) => void;
        onsampleselect: (item: SampleStructureItem) => void;
        onwavedataselect: (item: WaveDataItem) => void;
        onimportaudio?: () => void;
    }

    let {
        view,
        sampleBanks,
        samples,
        waveData,
        activeSampleBankId,
        activeSampleId,
        activeWaveDataId,
        queries,
        onquerychange,
        onsamplebankselect,
        onsampleselect,
        onwavedataselect,
        onimportaudio = () => undefined,
    }: Props = $props();

    function matches(name: string, query: string): boolean {
        return name.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase());
    }

    const sampleQuery = $derived(view === 'sample-banks' ? queries.secondary : queries.primary);
    const waveDataQuery = $derived(view === 'sample-banks' ? queries.tertiary : queries.secondary);
    const filteredBanks = $derived(sampleBanks.filter((item) => matches(item.name, queries.primary)));
    const filteredSamples = $derived(samples.filter((item) => matches(item.name, sampleQuery)));
    const filteredWaveData = $derived(waveData.filter((item) => matches(item.name, waveDataQuery)));
</script>

<section
    class:three-lanes={view === 'sample-banks'}
    class:two-lanes={view === 'samples'}
    class="contained-object-workspace"
    aria-label={view === 'sample-banks' ? 'Sample Bank hierarchy' : 'Sample hierarchy'}
>
    {#if view === 'sample-banks'}
        <section class="contained-lane">
            <CollectionToolbar
                title="Sample Banks (SBAC)"
                count={sampleBanks.length}
                query={queries.primary}
                placeholder="Search Sample Banks (SBAC)"
                onquerychange={(value) => onquerychange('primary', value)}
            />
            <div class="contained-list">
                {#each filteredBanks as item (item.id)}
                    <button
                        type="button"
                        class:active={activeSampleBankId === item.objectId}
                        aria-pressed={activeSampleBankId === item.objectId}
                        onclick={() => onsamplebankselect(item)}
                    >
                        <span>
                            <strong>{item.name}</strong>
                            <small>{item.memberCount ?? 0} {(item.memberCount ?? 0) === 1 ? 'Sample' : 'Samples'}</small
                            >
                        </span>
                    </button>
                {:else}
                    <p class="empty-copy">No matching Sample Banks</p>
                {/each}
            </div>
        </section>
    {/if}

    <section class="contained-lane">
        <CollectionToolbar
            title="Samples (SBNK)"
            count={samples.length}
            query={sampleQuery}
            placeholder="Search Samples"
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'secondary' : 'primary', value)}
            actionLabel={view === 'samples' ? 'Import audio' : undefined}
            onaction={onimportaudio}
        />
        <div class="contained-list">
            {#each filteredSamples as item (item.id)}
                <button
                    type="button"
                    class:active={activeSampleId === item.objectId}
                    aria-pressed={activeSampleId === item.objectId}
                    onclick={() => onsampleselect(item)}
                >
                    <span>
                        <strong>{item.name}</strong>
                        {#if view === 'samples'}<small>{item.membershipLabel ?? 'Standalone'}</small>{/if}
                    </span>
                </button>
            {:else}
                <p class="empty-copy">
                    {view === 'sample-banks' && !activeSampleBankId
                        ? 'Select a Sample Bank to inspect its Samples'
                        : 'No matching Samples'}
                </p>
            {/each}
        </div>
    </section>

    <section class="contained-lane">
        <CollectionToolbar
            title="Wave Data (SMPL)"
            count={waveData.length}
            query={waveDataQuery}
            placeholder="Search Wave Data"
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'tertiary' : 'secondary', value)}
        />
        <div class="contained-list">
            {#each filteredWaveData as item (item.id)}
                <button
                    type="button"
                    class:active={activeWaveDataId === item.objectKey}
                    aria-pressed={activeWaveDataId === item.objectKey}
                    onclick={() => onwavedataselect(item)}
                >
                    <span><strong>{item.name}</strong><small>{item.note} · {item.duration}</small></span>
                </button>
            {:else}
                <p class="empty-copy">
                    {!activeSampleId ? 'Select a Sample to inspect its Wave Data' : 'No matching Wave Data'}
                </p>
            {/each}
        </div>
    </section>
</section>
