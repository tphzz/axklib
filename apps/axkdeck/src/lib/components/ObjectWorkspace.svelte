<script lang="ts">
    import { onDestroy } from 'svelte';
    import { formatStoredSize } from '../formatBytes';
    import {
        emptyPackageExportSelection,
        selectionMode,
        type ObjectSelectionMode,
        updatePackageExportSelection,
        type PackageExportSelectionState,
    } from '../objectSelection';
    import { compareNamedItems } from '../naturalSort';
    import type { SamplerObject } from '../transport';
    import type { ObjectRenameTarget, PackageExportObject, Program, WaveDataItem, WorkspaceView } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import Icon from './Icon.svelte';
    import ObjectContextMenu from './ObjectContextMenu.svelte';
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
        objectRenameAvailable?: boolean;
        onrenameobject?: (target: ObjectRenameTarget) => void;
        objectDeletionAvailable?: boolean;
        ondeleteobjects?: (objects: PackageExportObject[]) => void;
        waveDataCleanupAvailable?: boolean;
        oncleanupwavedata?: () => void;
        packageExportAvailable?: boolean;
        onexportobjects?: (objects: PackageExportObject[]) => void;
        selection?: PackageExportSelectionState;
        onselectionchange?: (selection: PackageExportSelectionState) => void;
        onselectionlimit?: () => void;
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
        objectRenameAvailable = false,
        onrenameobject = () => undefined,
        objectDeletionAvailable = false,
        ondeleteobjects = () => undefined,
        waveDataCleanupAvailable = false,
        oncleanupwavedata = () => undefined,
        packageExportAvailable = false,
        onexportobjects = () => undefined,
        selection = emptyPackageExportSelection(),
        onselectionchange = () => undefined,
        onselectionlimit = () => undefined,
    }: Props = $props();
    let prefetchTimer: ReturnType<typeof setTimeout> | undefined;
    let objectMenu = $state<{
        target: SamplerObject;
        renameTarget: ObjectRenameTarget | null;
        objects: PackageExportObject[];
        left: number;
        top: number;
    } | null>(null);

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
        const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
        onseek(item, Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width)));
    }

    function exportObject(object: SamplerObject, name = object.name): PackageExportObject {
        const kind = object.objectType === 'PROG' ? 'PROGRAM' : (object.objectType as PackageExportObject['kind']);
        const typeLabel =
            kind === 'PROGRAM' ? 'Program' : kind === 'SMPL' ? 'Wave Data' : kind === 'SBAC' ? 'Sample Bank' : 'Sample';
        return {
            kind,
            objectId: object.key,
            name,
            typeLabel,
            partitionIndex: object.partitionIndex,
            partitionName: object.partitionName,
            volumeName: object.volumeName,
        };
    }

    function domainObjects(): PackageExportObject[] {
        return view === 'programs'
            ? programs.map((item) => exportObject(item.object, item.name))
            : orderedWaveData.map((item) => exportObject(item.object, item.name));
    }

    function visibleObjects(): PackageExportObject[] {
        return view === 'programs'
            ? filteredPrograms.map((item) => exportObject(item.object, item.name))
            : filteredWaveData.map((item) => exportObject(item.object, item.name));
    }

    function domainKey(): string {
        const first = domainObjects()[0];
        return `${view}\u0000${first?.partitionIndex ?? ''}\u0000${first?.volumeName ?? ''}`;
    }

    function updateSelection(event: MouseEvent, objectId: string): ObjectSelectionMode {
        const mode = selectionMode(event);
        const result = updatePackageExportSelection(
            selection,
            domainKey(),
            domainObjects(),
            visibleObjects(),
            objectId,
            mode,
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return mode;
    }

    function selectWaveData(event: MouseEvent, item: WaveDataItem, seekAfterSelection = false): void {
        if (updateSelection(event, item.objectKey) !== 'replace') return;
        onwavedataselect(item);
        if (seekAfterSelection) seek(event, item);
    }

    function clearWaveDataSelection(event: MouseEvent): void {
        if (
            view !== 'wave-data' ||
            event.button !== 0 ||
            event.ctrlKey ||
            event.metaKey ||
            event.shiftKey ||
            selection.items.length === 0
        ) {
            return;
        }
        const target = event.target;
        if (target instanceof Element && target.closest('.wave-data-row')) return;
        onselectionchange(emptyPackageExportSelection());
    }

    function selectAll(event: KeyboardEvent, objectId: string): boolean {
        if (!(event.ctrlKey || event.metaKey) || event.key.toLocaleLowerCase() !== 'a') return false;
        event.preventDefault();
        const result = updatePackageExportSelection(
            selection,
            domainKey(),
            domainObjects(),
            visibleObjects(),
            objectId,
            'all',
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return true;
    }

    function openObjectMenu(event: MouseEvent, object: SamplerObject, renameTarget: ObjectRenameTarget | null): void {
        if (!objectRenameAvailable && !objectDeletionAvailable && !packageExportAvailable) return;
        event.preventDefault();
        let menuSelection = selection;
        if (!selection.items.some((item) => item.objectId === object.key)) {
            const result = updatePackageExportSelection(
                selection,
                domainKey(),
                domainObjects(),
                visibleObjects(),
                object.key,
                'replace',
            );
            menuSelection = result.selection;
            onselectionchange(menuSelection);
        }
        objectMenu = {
            target: object,
            renameTarget,
            objects: menuSelection.items,
            left: Math.max(8, Math.min(event.clientX, window.innerWidth - 180)),
            top: Math.max(8, Math.min(event.clientY, window.innerHeight - 56)),
        };
    }

    function openObjectMenuFromKeyboard(
        event: KeyboardEvent,
        object: SamplerObject,
        renameTarget: ObjectRenameTarget | null,
    ): void {
        if (selectAll(event, object.key)) return;
        if (event.key !== 'ContextMenu' && !(event.shiftKey && event.key === 'F10')) return;
        if (!objectRenameAvailable && !objectDeletionAvailable && !packageExportAvailable) return;
        event.preventDefault();
        const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
        openObjectMenu(
            new MouseEvent('contextmenu', {
                clientX: bounds.left + Math.min(24, bounds.width / 2),
                clientY: bounds.top + Math.min(24, bounds.height / 2),
            }),
            object,
            renameTarget,
        );
    }

    function programRenameTarget(program: Program): ObjectRenameTarget | null {
        if (!/^\d{3}$/.test(program.slot)) return null;
        const programNumber = Number(program.slot);
        if (!Number.isInteger(programNumber) || programNumber < 1 || programNumber > 128) return null;
        return { kind: 'program', object: program.object, name: program.name, programNumber };
    }

    function waveDataRenameTarget(item: WaveDataItem): ObjectRenameTarget {
        return { kind: 'wave-data', object: item.object, name: item.name };
    }

    const title = $derived(view === 'programs' ? 'Programs' : 'Wave Data');
    const count = $derived(view === 'programs' ? programs.length : waveData.length);
    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const filteredPrograms = $derived(
        normalizedQuery
            ? programs.filter((item) => `${item.slot} ${item.name}`.toLocaleLowerCase().includes(normalizedQuery))
            : programs,
    );
    const orderedWaveData = $derived(waveData.toSorted(compareNamedItems));
    const filteredWaveData = $derived(
        normalizedQuery
            ? orderedWaveData.filter((item) => item.name.toLocaleLowerCase().includes(normalizedQuery))
            : orderedWaveData,
    );
    const emptyCollection = $derived(
        view === 'programs' ? filteredPrograms.length === 0 : filteredWaveData.length === 0,
    );
</script>

<section class="collection-panel" aria-label={title}>
    <CollectionToolbar
        {title}
        {count}
        {query}
        {onquerychange}
        actionLabel={view === 'wave-data' && waveDataCleanupAvailable ? 'Clean up unreferenced Wave Data' : undefined}
        actionIcon="broom"
        onaction={oncleanupwavedata}
    />
    <!-- Blank-space clearing is a pointer shortcut; the selection toolbar exposes the keyboard-accessible command. -->
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
        class:program-list={view === 'programs'}
        class:wave-data-list={view === 'wave-data'}
        class:empty-collection={emptyCollection}
        class="collection-body"
        onclick={clearWaveDataSelection}
    >
        {#if view === 'programs'}
            {#each filteredPrograms as program (program.id)}
                <button
                    type="button"
                    class:active={activeObjectId === program.objectId}
                    class:selected={selection.items.some((item) => item.objectId === program.objectId)}
                    class="program-row"
                    aria-pressed={activeObjectId === program.objectId}
                    onclick={(event) => {
                        updateSelection(event, program.objectId);
                        onprogramselect(program);
                    }}
                    oncontextmenu={(event) => openObjectMenu(event, program.object, programRenameTarget(program))}
                    onkeydown={(event) =>
                        openObjectMenuFromKeyboard(event, program.object, programRenameTarget(program))}
                >
                    <span class="object-slot">{program.slot}</span>
                    <strong>{program.name}</strong>
                </button>
            {:else}
                <p class="empty-copy">No matching Programs</p>
            {/each}
        {:else}
            {#each filteredWaveData as item (item.id)}
                <!-- The composite row owns its pointer context menu; the selection button retains the keyboard path. -->
                <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
                <div
                    use:observePreview={item}
                    class:active={activeObjectId === item.objectKey}
                    class:selected={selection.items.some((selected) => selected.objectId === item.objectKey)}
                    class="wave-data-row"
                    role="group"
                    aria-label={`${item.name} Wave Data`}
                    oncontextmenu={(event) => openObjectMenu(event, item.object, waveDataRenameTarget(item))}
                >
                    <button
                        class="wave-data-selection"
                        type="button"
                        aria-label={`Inspect ${item.name}`}
                        aria-pressed={selection.items.some((selected) => selected.objectId === item.objectKey)}
                        onclick={(event) => selectWaveData(event, item)}
                        onkeydown={(event) =>
                            openObjectMenuFromKeyboard(event, item.object, waveDataRenameTarget(item))}
                    ></button>
                    <strong class="wave-data-identity">{item.name}</strong>
                    <span class="wave-data-meta">{item.note} · {item.duration}</span>
                    <button
                        class="waveform-seek"
                        type="button"
                        aria-label={`Seek ${item.name}`}
                        onclick={(event) => selectWaveData(event, item, true)}
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
                        aria-label={playingObjectId === item.objectKey || preparingObjectId === item.objectKey
                            ? `Stop ${item.name}`
                            : `Play ${item.name}`}
                        title={preparingObjectId === item.objectKey
                            ? 'Stop preparing audio'
                            : playingObjectId === item.objectKey
                              ? 'Stop'
                              : 'Play'}
                        onpointerenter={() => schedulePrefetch(item)}
                        onpointerleave={clearPrefetch}
                        onfocus={() => schedulePrefetch(item)}
                        onblur={clearPrefetch}
                        onclick={(event) => {
                            event.stopPropagation();
                            clearPrefetch();
                            if (playingObjectId === item.objectKey || preparingObjectId === item.objectKey) onstop();
                            else onplay(item);
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
                <p class="empty-copy">No matching Wave Data</p>
            {/each}
        {/if}
    </div>
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
        onrename={objectRenameAvailable && objectMenu.objects.length === 1 && objectMenu.renameTarget
            ? () => onrenameobject(objectMenu!.renameTarget!)
            : undefined}
        onexport={packageExportAvailable ? () => onexportobjects(objectMenu!.objects) : undefined}
        ondelete={objectDeletionAvailable ? () => ondeleteobjects(objectMenu!.objects) : undefined}
    />
{/if}
