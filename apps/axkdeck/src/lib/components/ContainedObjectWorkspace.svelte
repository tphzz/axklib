<script lang="ts">
    import { matchesSearch } from '../auditionVisibility';
    import {
        emptyPackageExportSelection,
        selectionMode,
        updatePackageExportSelection,
        type PackageExportSelectionState,
    } from '../objectSelection';
    import { compareNamedItems } from '../naturalSort';
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
        selection?: PackageExportSelectionState;
        onselectionchange?: (selection: PackageExportSelectionState) => void;
        onselectionlimit?: () => void;
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
        selection = emptyPackageExportSelection(),
        onselectionchange = () => undefined,
        onselectionlimit = () => undefined,
    }: Props = $props();
    type SelectionScope = 'sample-banks' | 'samples' | 'wave-data';
    type SelectableItem = SampleStructureItem | WaveDataItem;
    let objectMenu = $state<{
        target: SamplerObject;
        objects: PackageExportObject[];
        left: number;
        top: number;
    } | null>(null);

    const sampleQuery = $derived(view === 'sample-banks' ? queries.secondary : queries.primary);
    const waveDataQuery = $derived(view === 'sample-banks' ? queries.tertiary : queries.secondary);
    const orderedBanks = $derived(sampleBanks.toSorted(compareNamedItems));
    const orderedSamples = $derived(samples.toSorted(compareNamedItems));
    const orderedWaveData = $derived(waveData.toSorted(compareNamedItems));
    const filteredBanks = $derived(orderedBanks.filter((item) => matchesSearch(item.name, queries.primary)));
    const filteredSamples = $derived(orderedSamples.filter((item) => matchesSearch(item.name, sampleQuery)));
    const filteredWaveData = $derived(orderedWaveData.filter((item) => matchesSearch(item.name, waveDataQuery)));

    function objectId(item: SelectableItem): string {
        return 'objectId' in item ? item.objectId : item.objectKey;
    }

    function exportObject(item: SelectableItem): PackageExportObject {
        const kind = item.object.objectType as PackageExportObject['kind'];
        const typeLabel = kind === 'SMPL' ? 'Wave Data' : kind === 'SBAC' ? 'Sample Bank' : 'Sample';
        return {
            kind,
            objectId: objectId(item),
            name: item.name,
            typeLabel,
            partitionIndex: item.object.partitionIndex,
            partitionName: item.object.partitionName,
            volumeName: item.object.volumeName,
        };
    }

    function selectionKey(scope: SelectionScope, domain: SelectableItem[]): string {
        const first = domain[0]?.object;
        const owner =
            scope === 'samples' && view === 'sample-banks'
                ? activeSampleBankId
                : scope === 'wave-data'
                  ? activeSampleId
                  : '';
        return `${view}\u0000${scope}\u0000${first?.partitionIndex ?? ''}\u0000${first?.volumeName ?? ''}\u0000${owner}`;
    }

    function updateSelection(
        event: MouseEvent,
        scope: SelectionScope,
        domain: SelectableItem[],
        visible: SelectableItem[],
        target: SelectableItem,
    ): void {
        const targetId = objectId(target);
        const result = updatePackageExportSelection(
            selection,
            selectionKey(scope, domain),
            domain.map(exportObject),
            visible.map(exportObject),
            targetId,
            selectionMode(event),
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
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
        const result = updatePackageExportSelection(
            selection,
            selectionKey(scope, domain),
            domain.map(exportObject),
            visible.map(exportObject),
            objectId(target),
            'all',
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return true;
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
        let menuSelection = selection;
        if (!packageExportAvailable || !selection.items.some((item) => item.objectId === targetId)) {
            const result = updatePackageExportSelection(
                selection,
                selectionKey(scope, domain),
                domain.map(exportObject),
                domain.map(exportObject),
                targetId,
                'replace',
            );
            menuSelection = result.selection;
            if (packageExportAvailable) onselectionchange(menuSelection);
        }
        objectMenu = {
            target: target.object,
            objects: menuSelection.items,
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
                    {@const playbackActive = playingSampleBankId === item.objectId}
                    {@const auditionable = auditionableSampleBankIds.has(item.objectId)}
                    <div
                        class="contained-row"
                        class:active={activeSampleBankId === item.objectId}
                        class:selected={selection.items.some((selected) => selected.objectId === item.objectId)}
                    >
                        <button
                            class="contained-identity"
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={activeSampleBankId === item.objectId}
                            onclick={(event) => {
                                updateSelection(event, 'sample-banks', orderedBanks, filteredBanks, item);
                                onsamplebankselect(item);
                            }}
                            oncontextmenu={(event) => openObjectMenu(event, 'sample-banks', orderedBanks, item)}
                            onkeydown={(event) =>
                                openObjectMenuFromKeyboard(event, 'sample-banks', orderedBanks, filteredBanks, item)}
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
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'secondary' : 'primary', value)}
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
                    class:selected={selection.items.some((selected) => selected.objectId === item.objectId)}
                >
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeSampleId === item.objectId}
                        onclick={(event) => {
                            updateSelection(event, 'samples', orderedSamples, filteredSamples, item);
                            onsampleselect(item);
                        }}
                        oncontextmenu={(event) => openObjectMenu(event, 'samples', orderedSamples, item)}
                        onkeydown={(event) =>
                            openObjectMenuFromKeyboard(event, 'samples', orderedSamples, filteredSamples, item)}
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
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'tertiary' : 'secondary', value)}
        />
        <div class="contained-list">
            {#each filteredWaveData as item (item.id)}
                <div
                    class="contained-row"
                    class:active={activeWaveDataId === item.objectKey}
                    class:selected={selection.items.some((selected) => selected.objectId === item.objectKey)}
                >
                    <button
                        class="contained-identity"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={activeWaveDataId === item.objectKey}
                        onclick={(event) => {
                            updateSelection(event, 'wave-data', orderedWaveData, filteredWaveData, item);
                            onwavedataselect(item);
                        }}
                        oncontextmenu={(event) => openObjectMenu(event, 'wave-data', orderedWaveData, item)}
                        onkeydown={(event) =>
                            openObjectMenuFromKeyboard(event, 'wave-data', orderedWaveData, filteredWaveData, item)}
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
