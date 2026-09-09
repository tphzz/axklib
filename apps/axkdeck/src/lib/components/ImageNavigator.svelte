<script lang="ts">
    import type { ImageLocation } from '../storageLocations';
    import type { ObjectSelectionMode } from '../objectSelection';
    import { orderSamplerTreeItems } from '../samplerTreeOrder';
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import Icon from './Icon.svelte';
    import ImageActions from './ImageActions.svelte';
    import ImageTreeContextMenu from './ImageTreeContextMenu.svelte';
    import TreeNode from './TreeNode.svelte';

    interface Props {
        image: ImageLocation | null;
        navigationOnly?: boolean;
        items: DiskTreeItem[];
        selectedId: string;
        selectedVolumeIds?: readonly string[];
        opening: boolean;
        storageLocationsAvailable: boolean;
        onopen: () => void;
        oncreate: () => void;
        onclose: () => void;
        onintegrity?: () => void;
        onmanagelocations: () => void;
        onselect: (item: DiskTreeItem, mode: ObjectSelectionMode, visibleVolumes: readonly DiskTreeItem[]) => void;
        oncontextselect?: (item: DiskTreeItem, visibleVolumes: readonly DiskTreeItem[]) => void;
        onloadchildren: (
            parentId: string,
            offset: number,
            limit: number,
        ) => Promise<{ items: DiskTreeItem[]; totalCount: number }>;
        volumeActionsEnabled: boolean;
        partitionActionsEnabled: boolean;
        packageImportEnabled?: boolean;
        packageExportEnabled?: boolean;
        volumePackageExportEnabled?: boolean;
        volumeFloppyExportEnabled?: boolean;
        audioExportEnabled?: boolean;
        mediaConversionEnabled?: boolean;
        allocationInspectionEnabled?: boolean;
        samplerOrderingEnabled?: boolean;
        onimageaction: (item: DiskTreeItem, action: ImageTreeAction) => void;
    }

    let {
        image,
        navigationOnly = false,
        items,
        selectedId,
        selectedVolumeIds = [],
        opening,
        storageLocationsAvailable,
        onopen,
        oncreate,
        onclose,
        onintegrity = () => undefined,
        onmanagelocations,
        onselect,
        oncontextselect = (item, visibleVolumes) => onselect(item, 'replace', visibleVolumes),
        onloadchildren,
        volumeActionsEnabled,
        partitionActionsEnabled,
        packageImportEnabled = false,
        packageExportEnabled = false,
        volumePackageExportEnabled = false,
        volumeFloppyExportEnabled = false,
        audioExportEnabled = false,
        mediaConversionEnabled = false,
        allocationInspectionEnabled = false,
        samplerOrderingEnabled = false,
        onimageaction,
    }: Props = $props();
    let filter = $state('');
    let treeMenu = $state<{
        item: DiskTreeItem;
        left: number;
        top: number;
        selectionCount: number;
    } | null>(null);
    let treeScroll = $state<HTMLElement>();
    const registeredItems = new Map<string, DiskTreeItem>();
    let loadedRootChildren = $state<DiskTreeItem[]>([]);
    let rootLoadError = $state(false);
    let rootLoadGeneration = 0;

    const contentItems = $derived.by(() => {
        if (items.length !== 1 || items[0]?.kind !== 'disk') return items;
        return items[0].children ?? loadedRootChildren;
    });
    const orderedContentItems = $derived(
        samplerOrderingEnabled ? orderSamplerTreeItems(contentItems, 'partition') : contentItems,
    );

    $effect(() => {
        const root = items.length === 1 && items[0]?.kind === 'disk' ? items[0] : null;
        const generation = ++rootLoadGeneration;
        loadedRootChildren = root?.children ?? [];
        rootLoadError = false;
        if (root && !root.children && root.childCount > 0) {
            void loadRootChildren(root.id, generation).catch(() => {
                if (generation === rootLoadGeneration) rootLoadError = true;
            });
        }
    });

    async function loadRootChildren(rootId: string, generation: number): Promise<void> {
        const result: DiskTreeItem[] = [];
        let offset = 0;
        for (let request = 0; request < 256; request += 1) {
            const page = await onloadchildren(rootId, offset, 200);
            if (generation !== rootLoadGeneration) return;
            if (page.items.length === 0) break;
            result.push(...page.items);
            offset += page.items.length;
            if (offset >= page.totalCount) break;
        }
        if (generation === rootLoadGeneration) loadedRootChildren = result;
    }

    function matches(item: DiskTreeItem, query: string): boolean {
        return (
            item.name.toLocaleLowerCase().includes(query) ||
            Boolean(item.children?.some((child) => matches(child, query)))
        );
    }

    const visibleItems = $derived.by(() => {
        const query = filter.trim().toLocaleLowerCase();
        return query ? orderedContentItems.filter((item) => matches(item, query)) : orderedContentItems;
    });

    function registerTreeItem(item: DiskTreeItem, present: boolean): void {
        if (present) registeredItems.set(item.id, item);
        else if (registeredItems.get(item.id) === item) registeredItems.delete(item.id);
    }

    function visibleVolumes(): DiskTreeItem[] {
        if (!treeScroll) return [];
        return Array.from(
            treeScroll.querySelectorAll<HTMLElement>('.tree-item-select[data-tree-kind="volume"]'),
        ).flatMap((element) => {
            const item = registeredItems.get(element.dataset.treeId ?? '');
            return item?.kind === 'volume' ? [item] : [];
        });
    }

    function selectTreeItem(item: DiskTreeItem, mode: ObjectSelectionMode): void {
        onselect(item, mode, visibleVolumes());
    }

    function requestTreeMenu(item: DiskTreeItem, x: number, y: number): void {
        const selected = item.kind === 'volume' && selectedVolumeIds.includes(item.id);
        const volumes = visibleVolumes();
        if (selected) oncontextselect(item, volumes);
        else onselect(item, 'replace', volumes);
        if (selected && selectedVolumeIds.length > 1 && !volumeActionsEnabled) return;
        treeMenu = {
            item,
            left: x,
            top: y,
            selectionCount: selected ? selectedVolumeIds.length : 1,
        };
    }

    function chooseTreeAction(action: ImageTreeAction): void {
        if (!treeMenu) return;
        const item = treeMenu.item;
        treeMenu = null;
        onimageaction(item, action);
    }

    function closeMenus(): void {
        treeMenu = null;
    }
</script>

<svelte:window onclick={closeMenus} onkeydown={(event) => event.key === 'Escape' && closeMenus()} />

<aside
    class="image-navigator"
    class:navigation-only={navigationOnly}
    aria-label={navigationOnly ? 'Device navigation' : 'Image navigator'}
>
    {#if !navigationOnly}
        <ImageActions
            {image}
            {opening}
            {storageLocationsAvailable}
            {onopen}
            {oncreate}
            {onclose}
            {onintegrity}
            {onmanagelocations}
        />
    {/if}
    <section class="image-contents" aria-label="Image contents">
        <div class="panel-heading">
            <div>
                <p class="eyebrow">Contents</p>
                <h2>Partitions and volumes</h2>
            </div>
        </div>

        {#if image}
            <label class="search-field mx-3 mb-2">
                <Icon name="search" size={15} />
                <input bind:value={filter} type="search" placeholder="Search" aria-label="Search image contents" />
            </label>
        {/if}

        <div
            bind:this={treeScroll}
            class="image-tree-scroll min-h-0 flex-1 overflow-y-auto px-2 pb-4"
            class:context-menu-open={treeMenu !== null}
            aria-label="Image contents tree"
            data-multiselectable="true"
        >
            {#if image}
                {#each visibleItems as item (item.id)}
                    <TreeNode
                        {item}
                        {selectedId}
                        {selectedVolumeIds}
                        onselect={selectTreeItem}
                        onregister={registerTreeItem}
                        {onloadchildren}
                        {volumeActionsEnabled}
                        {partitionActionsEnabled}
                        {packageImportEnabled}
                        {packageExportEnabled}
                        {volumePackageExportEnabled}
                        {volumeFloppyExportEnabled}
                        {audioExportEnabled}
                        {mediaConversionEnabled}
                        {allocationInspectionEnabled}
                        {samplerOrderingEnabled}
                        onrequestmenu={requestTreeMenu}
                    />
                {:else}
                    {#if rootLoadError}
                        <div class="contents-load-error">
                            <p>Image contents could not be loaded.</p>
                            <button
                                class="secondary-button"
                                type="button"
                                onclick={() => {
                                    const root = items[0];
                                    if (!root) return;
                                    const generation = ++rootLoadGeneration;
                                    rootLoadError = false;
                                    void loadRootChildren(root.id, generation).catch(() => {
                                        if (generation === rootLoadGeneration) rootLoadError = true;
                                    });
                                }}>Retry</button
                            >
                        </div>
                    {:else}
                        <p class="empty-copy">No matching contents</p>
                    {/if}
                {/each}
            {:else}
                <p class="empty-copy">Open an image to browse its contents</p>
            {/if}
        </div>
    </section>
</aside>

{#if treeMenu}
    {#key `${treeMenu.item.id}:${treeMenu.left}:${treeMenu.top}`}
        <ImageTreeContextMenu
            item={treeMenu.item}
            left={treeMenu.left}
            top={treeMenu.top}
            selectionCount={treeMenu.selectionCount}
            {volumeActionsEnabled}
            {partitionActionsEnabled}
            {packageImportEnabled}
            {packageExportEnabled}
            {volumePackageExportEnabled}
            {volumeFloppyExportEnabled}
            {audioExportEnabled}
            {mediaConversionEnabled}
            {allocationInspectionEnabled}
            onaction={chooseTreeAction}
            onclose={() => (treeMenu = null)}
        />
    {/key}
{/if}

<style>
    .navigation-only {
        display: contents;
    }
</style>
