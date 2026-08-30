<script lang="ts">
    import { matchesSearch } from '../auditionVisibility';
    import {
        collectionPageStep,
        focusCollectionIndex,
        hasDisallowedNavigationModifier,
        keyboardSelectionMode,
        linearNavigationIndex,
    } from '../collectionNavigation';
    import {
        emptyPackageExportSelection,
        selectionMode,
        type ObjectSelectionMode,
        updatePackageExportSelection,
    } from '../objectSelection';
    import { compareNamedItems } from '../naturalSort';
    import { orderedVisibleSamples } from '../sampleRelationships';
    import type { ObjectRenameTarget, PackageExportObject, SampleStructureItem, WaveDataItem } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import type {
        ContainedObjectMenuState,
        ContainedSelectableItem as SelectableItem,
        ContainedSelectionScope as SelectionScope,
    } from './containedObjectMenu';
    import Icon from './Icon.svelte';
    import ObjectContextMenu from './ObjectContextMenu.svelte';
    import ObjectSizeIdentity from './ObjectSizeIdentity.svelte';
    import type { ContainedObjectWorkspaceProps as Props } from './containedObjectWorkspaceProps';
    let {
        view,
        sampleBanks,
        samples,
        waveData,
        activeSampleBankId,
        activeSampleId,
        activeWaveDataId,
        queries,
        showOnlyStandaloneSamples = true,
        onshowonlystandalonechange = () => undefined,
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
        stereoSampleIds = new Set<string>(),
        objectRenameAvailable = false,
        onrenameobject = () => undefined,
        sampleBankAssignmentAvailable = false,
        onassignsamplebank = () => undefined,
        objectDeletionAvailable = false,
        ondeleteobjects = () => undefined,
        packageExportAvailable = false,
        onexportobjects = () => undefined,
        audioExportAvailable = false,
        onexportaudio = () => undefined,
        onexportwav = () => undefined,
        selection = emptyPackageExportSelection(),
        onselectionchange = () => undefined,
        onselectionlimit = () => undefined,
    }: Props = $props();
    let objectMenu = $state<ContainedObjectMenuState | null>(null);
    const sampleQuery = $derived(view === 'sample-banks' ? queries.secondary : queries.primary);
    const waveDataQuery = $derived(view === 'sample-banks' ? queries.tertiary : queries.secondary);
    const orderedBanks = $derived(sampleBanks.toSorted(compareNamedItems));
    const orderedWaveData = $derived(waveData.toSorted(compareNamedItems));
    const filteredBanks = $derived(orderedBanks.filter((item) => matchesSearch(item.name, queries.primary)));
    const availableSamples = $derived(orderedVisibleSamples(samples, view === 'samples' && showOnlyStandaloneSamples));
    const filteredSamples = $derived(availableSamples.filter((item) => matchesSearch(item.name, sampleQuery)));
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

    function renameTarget(item: SelectableItem): ObjectRenameTarget {
        if (item.object.objectType === 'SBAC') {
            return { kind: 'sample-bank', object: item.object, name: item.name };
        }
        if (item.object.objectType === 'SBNK') {
            return { kind: 'sample', object: item.object, name: item.name };
        }
        return { kind: 'wave-data', object: item.object, name: item.name };
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
        mode: ObjectSelectionMode,
        scope: SelectionScope,
        domain: SelectableItem[],
        visible: SelectableItem[],
        target: SelectableItem,
    ): ObjectSelectionMode {
        const targetId = objectId(target);
        const result = updatePackageExportSelection(
            selection,
            selectionKey(scope, domain),
            domain.map(exportObject),
            visible.map(exportObject),
            targetId,
            mode,
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return mode;
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
        if (
            !objectRenameAvailable &&
            !sampleBankAssignmentAvailable &&
            !objectDeletionAvailable &&
            !packageExportAvailable &&
            !audioExportAvailable
        )
            return;
        event.preventDefault();
        const targetId = objectId(target);
        let menuSelection = selection;
        if (!selection.items.some((item) => item.objectId === targetId)) {
            const result = updatePackageExportSelection(
                selection,
                selectionKey(scope, domain),
                domain.map(exportObject),
                domain.map(exportObject),
                targetId,
                'replace',
            );
            menuSelection = result.selection;
            onselectionchange(menuSelection);
        }
        const selectedIds = new Set(menuSelection.items.map((item) => item.objectId));
        const selectedSamples = domain.filter(
            (item): item is SampleStructureItem =>
                'objectType' in item && item.objectType === 'SBNK' && selectedIds.has(item.objectId),
        );
        const sampleBankAssignmentMembers =
            sampleBankAssignmentAvailable &&
            view === 'samples' &&
            scope === 'samples' &&
            selectedSamples.length === menuSelection.items.length &&
            selectedSamples.length <= 127
                ? selectedSamples
                : null;
        objectMenu = {
            directWav: scope !== 'sample-banks',
            renameTarget: renameTarget(target),
            objects: menuSelection.items,
            sampleBankAssignmentMembers,
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
        if (
            !objectRenameAvailable &&
            !sampleBankAssignmentAvailable &&
            !objectDeletionAvailable &&
            !packageExportAvailable &&
            !audioExportAvailable
        )
            return;
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

    function inspect(scope: SelectionScope, target: SelectableItem, mode: ObjectSelectionMode): void {
        const domain =
            scope === 'sample-banks' ? orderedBanks : scope === 'samples' ? availableSamples : orderedWaveData;
        const visible =
            scope === 'sample-banks' ? filteredBanks : scope === 'samples' ? filteredSamples : filteredWaveData;
        updateSelection(mode, scope, domain, visible, target);
        if (mode !== 'replace') return;
        if (scope === 'sample-banks') onsamplebankselect(target as SampleStructureItem);
        else if (scope === 'samples') onsampleselect(target as SampleStructureItem);
        else onwavedataselect(target as WaveDataItem);
    }

    function activeIndex(scope: SelectionScope, items: SelectableItem[]): number {
        const activeId =
            scope === 'sample-banks' ? activeSampleBankId : scope === 'samples' ? activeSampleId : activeWaveDataId;
        const index = items.findIndex((item) => objectId(item) === activeId);
        return index < 0 ? 0 : index;
    }

    function adjacentScope(scope: SelectionScope, direction: -1 | 1): SelectionScope | null {
        const scopes: SelectionScope[] =
            view === 'sample-banks' ? ['sample-banks', 'samples', 'wave-data'] : ['samples', 'wave-data'];
        return scopes[scopes.indexOf(scope) + direction] ?? null;
    }

    function visibleItems(scope: SelectionScope): SelectableItem[] {
        return scope === 'sample-banks' ? filteredBanks : scope === 'samples' ? filteredSamples : filteredWaveData;
    }

    function handleContainedKeyboard(
        event: KeyboardEvent,
        scope: SelectionScope,
        currentIndex: number,
        current: SelectableItem,
    ): void {
        if (!hasDisallowedNavigationModifier(event) && (event.key === 'ArrowLeft' || event.key === 'ArrowRight')) {
            const direction = event.key === 'ArrowLeft' ? -1 : 1;
            const targetScope = adjacentScope(scope, direction);
            const targets = targetScope ? visibleItems(targetScope) : [];
            if (targetScope && targets.length > 0) {
                event.preventDefault();
                const targetIndex = activeIndex(targetScope, targets);
                const target = targets[targetIndex];
                if (!target) return;
                inspect(targetScope, target, 'replace');
                void focusCollectionIndex(event.currentTarget, targetIndex, direction);
                return;
            }
        }
        if (!hasDisallowedNavigationModifier(event)) {
            const items = visibleItems(scope);
            const targetIndex = linearNavigationIndex(
                event.key,
                currentIndex,
                items.length,
                collectionPageStep(event.currentTarget),
            );
            if (targetIndex !== null) {
                event.preventDefault();
                if (targetIndex === currentIndex) return;
                const target = items[targetIndex];
                if (!target) return;
                inspect(scope, target, keyboardSelectionMode(event));
                void focusCollectionIndex(event.currentTarget, targetIndex);
                return;
            }
        }
        openObjectMenuFromKeyboard(
            event,
            scope,
            scope === 'sample-banks' ? orderedBanks : scope === 'samples' ? availableSamples : orderedWaveData,
            visibleItems(scope),
            current,
        );
    }
</script>

<section
    class:three-lanes={view === 'sample-banks'}
    class:two-lanes={view === 'samples'}
    class="contained-object-workspace"
    data-navigation-workspace
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
            <div class="contained-list" data-collection-list="sample-banks" data-navigation-list>
                {#each filteredBanks as item, index (item.id)}
                    {@const playbackActive = playingSampleBankId === item.objectId}
                    {@const auditionable = auditionableSampleBankIds.has(item.objectId)}
                    <div
                        class="contained-row"
                        class:active={activeSampleBankId === item.objectId}
                        class:selected={selection.items.some((selected) => selected.objectId === item.objectId)}
                    >
                        <button
                            class="contained-identity"
                            data-collection-object-id={item.objectId}
                            data-navigation-index={index}
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={selection.items.some((selected) => selected.objectId === item.objectId)}
                            onclick={(event) => {
                                if (
                                    updateSelection(
                                        selectionMode(event),
                                        'sample-banks',
                                        orderedBanks,
                                        filteredBanks,
                                        item,
                                    ) === 'replace'
                                ) {
                                    onsamplebankselect(item);
                                }
                            }}
                            oncontextmenu={(event) => openObjectMenu(event, 'sample-banks', orderedBanks, item)}
                            onkeydown={(event) => handleContainedKeyboard(event, 'sample-banks', index, item)}
                        >
                            <ObjectSizeIdentity
                                name={item.name}
                                object={item.object}
                                metadata={`${item.memberCount ?? 0} ${(item.memberCount ?? 0) === 1 ? 'Sample' : 'Samples'}`}
                            />
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
            count={availableSamples.length}
            query={sampleQuery}
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'secondary' : 'primary', value)}
            actions={view === 'samples' ? [{ label: 'Import audio', icon: 'upload', run: onimportaudio }] : []}
            filterLabel={view === 'samples' ? 'Show only standalone' : undefined}
            filterChecked={showOnlyStandaloneSamples}
            onfilterchange={onshowonlystandalonechange}
        />
        <div class="contained-list" data-collection-list="samples" data-navigation-list>
            {#if filteredSamples.length > 0}
                {#each filteredSamples as item, index (item.id)}
                    {@const playbackActive = playingObjectId === item.objectId || preparingObjectId === item.objectId}
                    {@const auditionable = auditionableSampleIds.has(item.objectId)}
                    <div
                        class="contained-row"
                        class:active={activeSampleId === item.objectId}
                        class:selected={selection.items.some((selected) => selected.objectId === item.objectId)}
                    >
                        <button
                            class="contained-identity"
                            data-collection-object-id={item.objectId}
                            data-navigation-index={index}
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={selection.items.some((selected) => selected.objectId === item.objectId)}
                            onclick={(event) => {
                                if (
                                    updateSelection(
                                        selectionMode(event),
                                        'samples',
                                        availableSamples,
                                        filteredSamples,
                                        item,
                                    ) === 'replace'
                                ) {
                                    onsampleselect(item);
                                }
                            }}
                            oncontextmenu={(event) => openObjectMenu(event, 'samples', availableSamples, item)}
                            onkeydown={(event) => handleContainedKeyboard(event, 'samples', index, item)}
                        >
                            <ObjectSizeIdentity
                                name={item.name}
                                object={item.object}
                                metadata={view === 'samples' ? (item.membershipLabel ?? 'Standalone') : ''}
                                indicator={stereoSampleIds.has(item.objectId) ? 'stereo' : undefined}
                            />
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
                {/each}
            {:else}
                <p class="empty-copy">
                    {view === 'sample-banks' && !activeSampleBankId
                        ? 'Select a Sample Bank to inspect its Samples'
                        : sampleQuery
                          ? 'No matching Samples'
                          : view === 'samples' && showOnlyStandaloneSamples
                            ? 'No standalone Samples'
                            : 'No Samples'}
                </p>
            {/if}
        </div>
    </section>

    <section class="contained-lane">
        <CollectionToolbar
            title="Wave Data"
            count={waveData.length}
            query={waveDataQuery}
            onquerychange={(value) => onquerychange(view === 'sample-banks' ? 'tertiary' : 'secondary', value)}
        />
        <div class="contained-list" data-collection-list="wave-data" data-navigation-list>
            {#if filteredWaveData.length > 0}
                {#each filteredWaveData as item, index (item.id)}
                    <div
                        class="contained-row"
                        class:active={activeWaveDataId === item.objectKey}
                        class:selected={selection.items.some((selected) => selected.objectId === item.objectKey)}
                    >
                        <button
                            class="contained-identity"
                            data-collection-object-id={item.objectKey}
                            data-navigation-index={index}
                            type="button"
                            aria-label={`Inspect ${item.name}`}
                            aria-pressed={selection.items.some((selected) => selected.objectId === item.objectKey)}
                            onclick={(event) => {
                                if (
                                    updateSelection(
                                        selectionMode(event),
                                        'wave-data',
                                        orderedWaveData,
                                        filteredWaveData,
                                        item,
                                    ) === 'replace'
                                ) {
                                    onwavedataselect(item);
                                }
                            }}
                            oncontextmenu={(event) => openObjectMenu(event, 'wave-data', orderedWaveData, item)}
                            onkeydown={(event) => handleContainedKeyboard(event, 'wave-data', index, item)}
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
                                if (playingObjectId === item.objectKey || preparingObjectId === item.objectKey)
                                    onstop();
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
                {/each}
            {:else}
                <p class="empty-copy">
                    {!activeSampleId ? 'Select a Sample to inspect its Wave Data' : 'No matching Wave Data'}
                </p>
            {/if}
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
        onrename={objectRenameAvailable && objectMenu.objects.length === 1
            ? () => onrenameobject(objectMenu!.renameTarget)
            : undefined}
        onassignsamplebank={objectMenu.sampleBankAssignmentMembers
            ? () => onassignsamplebank(objectMenu!.sampleBankAssignmentMembers!)
            : undefined}
        onexportpackage={packageExportAvailable ? () => onexportobjects(objectMenu!.objects) : undefined}
        onexportwav={audioExportAvailable && objectMenu.directWav ? () => onexportwav(objectMenu!.objects) : undefined}
        onexportsfz={audioExportAvailable ? () => onexportaudio(objectMenu!.objects) : undefined}
        ondelete={objectDeletionAvailable ? () => ondeleteobjects(objectMenu!.objects) : undefined}
    />
{/if}
