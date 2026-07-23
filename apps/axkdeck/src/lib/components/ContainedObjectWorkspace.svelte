<script lang="ts">
    import type { SampleStructureItem, WaveDataItem } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import Icon from './Icon.svelte';

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
        onplaysamplebank?: (item: SampleStructureItem) => void;
        onplaysample?: (item: SampleStructureItem) => void;
        onplaywavedata?: (item: WaveDataItem) => void;
        onstop?: () => void;
        onimportaudio?: () => void;
        playingSampleBankId?: string;
        playingObjectId?: string | null;
        preparingObjectId?: string | null;
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
        onplaysamplebank = () => undefined,
        onplaysample = () => undefined,
        onplaywavedata = () => undefined,
        onstop = () => undefined,
        onimportaudio = () => undefined,
        playingSampleBankId = '',
        playingObjectId = null,
        preparingObjectId = null,
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
                title="Sample Banks"
                count={sampleBanks.length}
                query={queries.primary}
                onquerychange={(value) => onquerychange('primary', value)}
            />
            <div class="contained-list">
                {#each filteredBanks as item (item.id)}
                    <div class="contained-row" class:active={activeSampleBankId === item.objectId}>
                        <button
                            class="contained-identity"
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={activeSampleBankId === item.objectId}
                            onclick={() => onsamplebankselect(item)}
                        >
                            <strong>{item.name}</strong>
                            <small>{item.memberCount ?? 0} {(item.memberCount ?? 0) === 1 ? 'Sample' : 'Samples'}</small
                            >
                        </button>
                        <button
                            class="contained-playback icon-button"
                            type="button"
                            aria-label={playingSampleBankId === item.objectId
                                ? `Stop ${item.name}`
                                : `Play ${item.name}`}
                            title={playingSampleBankId === item.objectId ? 'Stop' : 'Play'}
                            onclick={() => {
                                if (playingSampleBankId === item.objectId) onstop();
                                else onplaysamplebank(item);
                            }}
                        >
                            <Icon name={playingSampleBankId === item.objectId ? 'stop' : 'play'} size={13} />
                        </button>
                    </div>
                {:else}
                    <p class="empty-copy">No matching Sample Banks</p>
                {/each}
            </div>
        </section>
    {/if}

    <section class="contained-lane">
        <CollectionToolbar
            title="Samples"
            count={samples.length}
            query={sampleQuery}
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'secondary' : 'primary', value)}
            actionLabel={view === 'samples' ? 'Import audio' : undefined}
            onaction={onimportaudio}
        />
        <div class="contained-list">
            {#each filteredSamples as item (item.id)}
                <div class="contained-row" class:active={activeSampleId === item.objectId}>
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeSampleId === item.objectId}
                        onclick={() => onsampleselect(item)}
                    >
                        <strong>{item.name}</strong>
                        {#if view === 'samples'}<small>{item.membershipLabel ?? 'Standalone'}</small>{/if}
                    </button>
                    <button
                        class="contained-playback icon-button"
                        type="button"
                        aria-label={playingObjectId === item.objectId ? `Stop ${item.name}` : `Play ${item.name}`}
                        title={preparingObjectId === item.objectId
                            ? 'Preparing audio'
                            : playingObjectId === item.objectId
                              ? 'Stop'
                              : 'Play'}
                        disabled={preparingObjectId === item.objectId}
                        onclick={() => {
                            if (playingObjectId === item.objectId) onstop();
                            else onplaysample(item);
                        }}
                    >
                        <Icon name={playingObjectId === item.objectId ? 'stop' : 'play'} size={13} />
                    </button>
                </div>
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
            title="Wave Data"
            count={waveData.length}
            query={waveDataQuery}
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'tertiary' : 'secondary', value)}
        />
        <div class="contained-list">
            {#each filteredWaveData as item (item.id)}
                <div class="contained-row" class:active={activeWaveDataId === item.objectKey}>
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeWaveDataId === item.objectKey}
                        onclick={() => onwavedataselect(item)}
                    >
                        <strong>{item.name}</strong><small>{item.note} · {item.duration}</small>
                    </button>
                    <button
                        class="contained-playback icon-button"
                        type="button"
                        aria-label={playingObjectId === item.objectKey ? `Stop ${item.name}` : `Play ${item.name}`}
                        title={preparingObjectId === item.objectKey
                            ? 'Preparing audio'
                            : playingObjectId === item.objectKey
                              ? 'Stop'
                              : 'Play'}
                        disabled={preparingObjectId === item.objectKey}
                        onclick={() => {
                            if (playingObjectId === item.objectKey) onstop();
                            else onplaywavedata(item);
                        }}
                    >
                        <Icon name={playingObjectId === item.objectKey ? 'stop' : 'play'} size={13} />
                    </button>
                </div>
            {:else}
                <p class="empty-copy">
                    {!activeSampleId ? 'Select a Sample to inspect its Wave Data' : 'No matching Wave Data'}
                </p>
            {/each}
        </div>
    </section>
</section>
