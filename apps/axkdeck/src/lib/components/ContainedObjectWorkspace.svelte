<script lang="ts">
    import { matchesSearch } from '../auditionVisibility';
    import { selectionMode, updateObjectSelection } from '../objectSelection';
    import type { SamplerObject } from '../transport';
    import type { PackageExportObject, SampleStructureItem, WaveDataItem } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import Icon from './Icon.svelte';
    import ObjectContextMenu from './ObjectContextMenu.svelte';

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
        auditionableSampleIds: ReadonlySet<string>;
        auditionableSampleBankIds: ReadonlySet<string>;
        objectDeletionAvailable?: boolean;
        ondeleteobject?: (object: SamplerObject) => void;
        packageExportAvailable?: boolean;
        onexportobjects?: (objects: PackageExportObject[]) => void;
        selectionEpoch?: number;
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
        auditionableSampleIds,
        auditionableSampleBankIds,
        objectDeletionAvailable = false,
        ondeleteobject = () => undefined,
        packageExportAvailable = false,
        onexportobjects = () => undefined,
        selectionEpoch = 0,
    }: Props = $props();
    type SelectionScope = 'sample-banks' | 'samples' | 'wave-data';
    type SelectableItem = SampleStructureItem | WaveDataItem;
    let selectionScope = $state<SelectionScope | null>(null);
    let selectedObjectIds = $state<string[]>([]);
    let selectionAnchorId = $state('');
    let selectionResetKey = '';
    let objectMenu = $state<{
        target: SamplerObject;
        objects: PackageExportObject[];
        left: number;
        top: number;
    } | null>(null);

    const sampleQuery = $derived(view === 'sample-banks' ? queries.secondary : queries.primary);
    const waveDataQuery = $derived(view === 'sample-banks' ? queries.tertiary : queries.secondary);
    const filteredBanks = $derived(sampleBanks.filter((item) => matchesSearch(item.name, queries.primary)));
    const filteredSamples = $derived(samples.filter((item) => matchesSearch(item.name, sampleQuery)));
    const filteredWaveData = $derived(waveData.filter((item) => matchesSearch(item.name, waveDataQuery)));

    function objectId(item: SelectableItem): string {
        return 'objectId' in item ? item.objectId : item.objectKey;
    }

    function exportObject(item: SelectableItem): PackageExportObject {
        const kind = item.object.objectType as PackageExportObject['kind'];
        const typeLabel = kind === 'SMPL' ? 'Wave Data' : kind === 'SBAC' ? 'Sample Bank' : 'Sample';
        return { kind, objectId: objectId(item), name: item.name, typeLabel };
    }

    function selectionKey(scope: SelectionScope): string {
        const [items, query] =
            scope === 'sample-banks'
                ? [sampleBanks, queries.primary]
                : scope === 'samples'
                  ? [samples, sampleQuery]
                  : [waveData, waveDataQuery];
        return `${selectionEpoch}\u0000${view}\u0000${scope}\u0000${query}\u0000${items.map(objectId).join('\u0000')}`;
    }

    function updateSelection(
        event: MouseEvent,
        scope: SelectionScope,
        domain: SelectableItem[],
        visible: SelectableItem[],
        target: SelectableItem,
    ): void {
        const targetId = objectId(target);
        const result = updateObjectSelection(
            selectionScope === scope ? selectedObjectIds : [],
            selectionScope === scope ? selectionAnchorId : '',
            domain.map(objectId),
            visible.map(objectId),
            targetId,
            selectionMode(event),
        );
        selectionScope = scope;
        selectedObjectIds = result.objectIds;
        selectionAnchorId = result.anchorId;
        selectionResetKey = selectionKey(scope);
    }

    function selectAll(
        event: KeyboardEvent,
        scope: SelectionScope,
        domain: SelectableItem[],
        visible: SelectableItem[],
        target: SelectableItem,
    ): boolean {
        if (!(event.ctrlKey || event.metaKey) || event.key.toLocaleLowerCase() !== 'a') return false;
        event.preventDefault();
        const result = updateObjectSelection(
            selectionScope === scope ? selectedObjectIds : [],
            selectionScope === scope ? selectionAnchorId : '',
            domain.map(objectId),
            visible.map(objectId),
            objectId(target),
            'all',
        );
        selectionScope = scope;
        selectedObjectIds = result.objectIds;
        selectionAnchorId = result.anchorId;
        selectionResetKey = selectionKey(scope);
        return true;
    }

    function clearSelection(): void {
        selectionScope = null;
        selectedObjectIds = [];
        selectionAnchorId = '';
        selectionResetKey = '';
        objectMenu = null;
    }

    function openObjectMenu(
        event: MouseEvent,
        scope: SelectionScope,
        domain: SelectableItem[],
        target: SelectableItem,
    ): void {
        if (!objectDeletionAvailable && !packageExportAvailable) return;
        event.preventDefault();
        const targetId = objectId(target);
        if (!packageExportAvailable || selectionScope !== scope || !selectedObjectIds.includes(targetId)) {
            selectionScope = scope;
            selectedObjectIds = [targetId];
            selectionAnchorId = targetId;
            selectionResetKey = selectionKey(scope);
        }
        const selected = new Set(selectedObjectIds);
        objectMenu = {
            target: target.object,
            objects: domain.filter((item) => selected.has(objectId(item))).map(exportObject),
            left: Math.max(8, Math.min(event.clientX, window.innerWidth - 180)),
            top: Math.max(8, Math.min(event.clientY, window.innerHeight - 56)),
        };
    }

    function openObjectMenuFromKeyboard(
        event: KeyboardEvent,
        scope: SelectionScope,
        domain: SelectableItem[],
        visible: SelectableItem[],
        target: SelectableItem,
    ): void {
        if (selectAll(event, scope, domain, visible, target)) return;
        if (event.key !== 'ContextMenu' && !(event.shiftKey && event.key === 'F10')) return;
        if (!objectDeletionAvailable && !packageExportAvailable) return;
        event.preventDefault();
        const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
        openObjectMenu(
            new MouseEvent('contextmenu', {
                clientX: bounds.left + Math.min(24, bounds.width / 2),
                clientY: bounds.top + Math.min(24, bounds.height / 2),
            }),
            scope,
            domain,
            target,
        );
    }

    $effect(() => {
        if (!selectionScope) return;
        const nextKey = selectionKey(selectionScope);
        if (selectionResetKey && nextKey !== selectionResetKey) clearSelection();
    });
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
                onquerychange={(value) => {
                    clearSelection();
                    onquerychange('primary', value);
                }}
            />
            <div class="contained-list">
                {#each filteredBanks as item (item.id)}
                    {@const playbackActive = playingSampleBankId === item.objectId}
                    {@const auditionable = auditionableSampleBankIds.has(item.objectId)}
                    <div
                        class="contained-row"
                        class:active={activeSampleBankId === item.objectId}
                        class:selected={selectionScope === 'sample-banks' && selectedObjectIds.includes(item.objectId)}
                    >
                        <button
                            class="contained-identity"
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={activeSampleBankId === item.objectId}
                            onclick={(event) => {
                                updateSelection(event, 'sample-banks', sampleBanks, filteredBanks, item);
                                onsamplebankselect(item);
                            }}
                            oncontextmenu={(event) => openObjectMenu(event, 'sample-banks', sampleBanks, item)}
                            onkeydown={(event) =>
                                openObjectMenuFromKeyboard(event, 'sample-banks', sampleBanks, filteredBanks, item)}
                        >
                            <strong>{item.name}</strong>
                            <small>{item.memberCount ?? 0} {(item.memberCount ?? 0) === 1 ? 'Sample' : 'Samples'}</small
                            >
                        </button>
                        <button
                            class="contained-playback icon-button"
                            type="button"
                            disabled={!playbackActive && !auditionable}
                            aria-label={playbackActive
                                ? `Stop ${item.name}`
                                : auditionable
                                  ? `Play ${item.name}`
                                  : `${item.name} cannot be auditioned`}
                            title={playbackActive
                                ? 'Stop'
                                : auditionable
                                  ? 'Play'
                                  : 'No Samples with confirmed Wave Data'}
                            onclick={() => {
                                if (playbackActive) onstop();
                                else if (auditionable) onplaysamplebank(item);
                            }}
                        >
                            <Icon name={playbackActive ? 'stop' : 'play'} size={13} />
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
            onquerychange={(value) => {
                clearSelection();
                onquerychange(view === 'sample-banks' ? 'secondary' : 'primary', value);
            }}
            actionLabel={view === 'samples' ? 'Import audio' : undefined}
            onaction={onimportaudio}
        />
        <div class="contained-list">
            {#each filteredSamples as item (item.id)}
                {@const playbackActive = playingObjectId === item.objectId || preparingObjectId === item.objectId}
                {@const auditionable = auditionableSampleIds.has(item.objectId)}
                <div
                    class="contained-row"
                    class:active={activeSampleId === item.objectId}
                    class:selected={selectionScope === 'samples' && selectedObjectIds.includes(item.objectId)}
                >
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeSampleId === item.objectId}
                        onclick={(event) => {
                            updateSelection(event, 'samples', samples, filteredSamples, item);
                            onsampleselect(item);
                        }}
                        oncontextmenu={(event) => openObjectMenu(event, 'samples', samples, item)}
                        onkeydown={(event) =>
                            openObjectMenuFromKeyboard(event, 'samples', samples, filteredSamples, item)}
                    >
                        <strong>{item.name}</strong>
                        {#if view === 'samples'}<small>{item.membershipLabel ?? 'Standalone'}</small>{/if}
                    </button>
                    <button
                        class="contained-playback icon-button"
                        type="button"
                        disabled={!playbackActive && !auditionable}
                        aria-label={playbackActive
                            ? `Stop ${item.name}`
                            : auditionable
                              ? `Play ${item.name}`
                              : `${item.name} cannot be auditioned`}
                        title={preparingObjectId === item.objectId
                            ? 'Stop preparing audio'
                            : playingObjectId === item.objectId
                              ? 'Stop'
                              : auditionable
                                ? 'Play'
                                : 'No confirmed Wave Data'}
                        onclick={() => {
                            if (playbackActive) onstop();
                            else if (auditionable) onplaysample(item);
                        }}
                    >
                        <Icon name={playbackActive ? 'stop' : 'play'} size={13} />
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
            onquerychange={(value) => {
                clearSelection();
                onquerychange(view === 'sample-banks' ? 'tertiary' : 'secondary', value);
            }}
        />
        <div class="contained-list">
            {#each filteredWaveData as item (item.id)}
                <div
                    class="contained-row"
                    class:active={activeWaveDataId === item.objectKey}
                    class:selected={selectionScope === 'wave-data' && selectedObjectIds.includes(item.objectKey)}
                >
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeWaveDataId === item.objectKey}
                        onclick={(event) => {
                            updateSelection(event, 'wave-data', waveData, filteredWaveData, item);
                            onwavedataselect(item);
                        }}
                        oncontextmenu={(event) => openObjectMenu(event, 'wave-data', waveData, item)}
                        onkeydown={(event) =>
                            openObjectMenuFromKeyboard(event, 'wave-data', waveData, filteredWaveData, item)}
                    >
                        <strong>{item.name}</strong><small>{item.note} · {item.duration}</small>
                    </button>
                    <button
                        class="contained-playback icon-button"
                        type="button"
                        aria-label={playingObjectId === item.objectKey || preparingObjectId === item.objectKey
                            ? `Stop ${item.name}`
                            : `Play ${item.name}`}
                        title={preparingObjectId === item.objectKey
                            ? 'Stop preparing audio'
                            : playingObjectId === item.objectKey
                              ? 'Stop'
                              : 'Play'}
                        onclick={() => {
                            if (playingObjectId === item.objectKey || preparingObjectId === item.objectKey) onstop();
                            else onplaywavedata(item);
                        }}
                    >
                        <Icon
                            name={playingObjectId === item.objectKey || preparingObjectId === item.objectKey
                                ? 'stop'
                                : 'play'}
                            size={13}
                        />
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

{#if objectMenu}
    <ObjectContextMenu
        objectName={objectMenu.objects.length === 1
            ? objectMenu.objects[0]!.name
            : `${objectMenu.objects.length} objects`}
        selectionCount={objectMenu.objects.length}
        left={objectMenu.left}
        top={objectMenu.top}
        onclose={() => (objectMenu = null)}
        onexport={packageExportAvailable ? () => onexportobjects(objectMenu!.objects) : undefined}
        ondelete={objectDeletionAvailable && objectMenu.objects.length === 1
            ? () => ondeleteobject(objectMenu!.target)
            : undefined}
    />
{/if}
