<script lang="ts">
    import { onDestroy } from 'svelte';
    import { formatStoredSize } from '../formatBytes';
    import type { Program, WaveDataItem, WorkspaceView } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import Icon from './Icon.svelte';
    import Waveform from './Waveform.svelte';

    interface Props {
        programs: Program[];
        waveData: WaveDataItem[];
        view: WorkspaceView;
        activeObjectId: string;
        query: string;
        onquerychange: (value: string) => void;
        onprogramselect: (program: Program) => void;
        onwavedataselect: (item: WaveDataItem) => void;
        onpreviewrequest?: (item: WaveDataItem) => void;
        onplay?: (item: WaveDataItem) => void;
        onprefetch?: (item: WaveDataItem) => void;
        onstop?: () => void;
        onseek?: (item: WaveDataItem, ratio: number) => void;
        playingObjectId?: string | null;
        preparingObjectId?: string | null;
        playheadFrame?: number;
    }

    let {
        programs,
        waveData,
        view,
        activeObjectId,
        query,
        onquerychange,
        onprogramselect,
        onwavedataselect,
        onpreviewrequest = () => undefined,
        onplay = () => undefined,
        onprefetch = () => undefined,
        onstop = () => undefined,
        onseek = () => undefined,
        playingObjectId = null,
        preparingObjectId = null,
        playheadFrame = 0,
    }: Props = $props();
    let prefetchTimer: ReturnType<typeof setTimeout> | undefined;

    onDestroy(() => clearPrefetch());

    function schedulePrefetch(item: WaveDataItem): void {
        clearPrefetch();
        prefetchTimer = setTimeout(() => {
            prefetchTimer = undefined;
            onprefetch(item);
        }, 150);
    }

    function clearPrefetch(): void {
        if (prefetchTimer !== undefined) clearTimeout(prefetchTimer);
        prefetchTimer = undefined;
    }

    function observePreview(node: HTMLElement, item: WaveDataItem): { destroy: () => void } {
        if (typeof IntersectionObserver === 'undefined') {
            onpreviewrequest(item);
            return { destroy: () => undefined };
        }
        const observer = new IntersectionObserver(
            (entries) => {
                if (entries.some((entry) => entry.isIntersecting)) onpreviewrequest(item);
            },
            { root: node.closest('.collection-body'), rootMargin: '80px' },
        );
        observer.observe(node);
        return { destroy: () => observer.disconnect() };
    }

    function seek(event: MouseEvent, item: WaveDataItem): void {
        event.stopPropagation();
        const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
        onseek(item, Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width)));
    }

    const title = $derived(view === 'programs' ? 'Programs' : 'Wave Data (SMPL)');
    const count = $derived(view === 'programs' ? programs.length : waveData.length);
    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const filteredPrograms = $derived(
        normalizedQuery ? programs.filter((item) => item.name.toLocaleLowerCase().includes(normalizedQuery)) : programs,
    );
    const filteredWaveData = $derived(
        normalizedQuery ? waveData.filter((item) => item.name.toLocaleLowerCase().includes(normalizedQuery)) : waveData,
    );
    const emptyCollection = $derived(
        view === 'programs' ? filteredPrograms.length === 0 : filteredWaveData.length === 0,
    );
</script>

<section class="collection-panel" aria-label={title}>
    <CollectionToolbar {title} {count} {query} placeholder={`Search ${title}`} {onquerychange} />
    <div
        class:object-grid={view !== 'wave-data'}
        class:wave-data-list={view === 'wave-data'}
        class:empty-collection={emptyCollection}
        class="collection-body"
    >
        {#if view === 'programs'}
            {#each filteredPrograms as program (program.id)}
                <button
                    type="button"
                    class:active={activeObjectId === program.objectId}
                    class="object-card"
                    onclick={() => onprogramselect(program)}
                >
                    <span class="object-slot">{program.slot}</span>
                    <strong>{program.name}</strong>
                </button>
            {:else}
                <p class="empty-copy">No matching Programs</p>
            {/each}
        {:else}
            {#each filteredWaveData as item (item.id)}
                <div use:observePreview={item} class:active={activeObjectId === item.objectKey} class="wave-data-row">
                    <button
                        class="wave-data-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        onclick={() => onwavedataselect(item)}
                    >
                        <strong>{item.name}</strong>
                    </button>
                    <span class="wave-data-meta">{item.note} · {item.duration}</span>
                    <button
                        class="waveform-seek"
                        type="button"
                        aria-label={`Seek ${item.name}`}
                        onclick={(event) => {
                            onwavedataselect(item);
                            seek(event, item);
                        }}
                    >
                        <Waveform
                            values={item.waveform}
                            playheadRatio={playingObjectId === item.objectKey && item.object.frameCount > 0
                                ? playheadFrame / item.object.frameCount
                                : 0}
                        />
                    </button>
                    <span class="wave-data-format"
                        >{item.sampleRate} · {item.bitDepth} · {formatStoredSize(item.storedSizeBytes)}</span
                    >
                    <button
                        class="wave-data-playback icon-button"
                        type="button"
                        aria-label={playingObjectId === item.objectKey ? `Stop ${item.name}` : `Play ${item.name}`}
                        title={preparingObjectId === item.objectKey
                            ? 'Preparing audio'
                            : playingObjectId === item.objectKey
                              ? 'Stop'
                              : 'Play'}
                        disabled={preparingObjectId === item.objectKey}
                        onpointerenter={() => schedulePrefetch(item)}
                        onpointerleave={clearPrefetch}
                        onfocus={() => schedulePrefetch(item)}
                        onblur={clearPrefetch}
                        onclick={() => {
                            clearPrefetch();
                            if (playingObjectId === item.objectKey) onstop();
                            else onplay(item);
                        }}
                    >
                        <Icon name={playingObjectId === item.objectKey ? 'stop' : 'play'} size={13} />
                    </button>
                </div>
            {:else}
                <p class="empty-copy">No matching Wave Data</p>
            {/each}
        {/if}
    </div>
</section>
