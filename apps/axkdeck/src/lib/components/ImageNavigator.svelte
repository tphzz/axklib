<script lang="ts">
    import type { ImageLocation } from '../storageLocations';
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import Icon from './Icon.svelte';
    import TreeNode from './TreeNode.svelte';

    interface Props {
        image: ImageLocation | null;
        items: DiskTreeItem[];
        selectedId: string;
        opening: boolean;
        storageLocationsAvailable: boolean;
        onopen: () => void;
        oncreate: () => void;
        onclose: () => void;
        onmanagelocations: () => void;
        onselect: (item: DiskTreeItem) => void;
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
        onimageaction: (item: DiskTreeItem, action: ImageTreeAction) => void;
    }

    let {
        image,
        items,
        selectedId,
        opening,
        storageLocationsAvailable,
        onopen,
        oncreate,
        onclose,
        onmanagelocations,
        onselect,
        onloadchildren,
        volumeActionsEnabled,
        partitionActionsEnabled,
        packageImportEnabled = false,
        packageExportEnabled = false,
        volumePackageExportEnabled = false,
        volumeFloppyExportEnabled = false,
        audioExportEnabled = false,
        mediaConversionEnabled = false,
        onimageaction,
    }: Props = $props();
    let filter = $state('');
    let imageMenuOpen = $state(false);
    let treeMenu = $state<{ item: DiskTreeItem; left: number; top: number } | null>(null);
    let loadedRootChildren = $state<DiskTreeItem[]>([]);
    let rootLoadError = $state(false);
    let rootLoadGeneration = 0;

    const pathParts = $derived(image?.displayName.replaceAll('\\', '/').split('/').filter(Boolean) ?? []);
    const imageName = $derived(pathParts.at(-1) ?? 'No image open');
    const imageLocation = $derived(pathParts.slice(0, -1).join('/'));
    const contentItems = $derived.by(() => {
        if (items.length !== 1 || items[0]?.kind !== 'disk') return items;
        return items[0].children ?? loadedRootChildren;
    });

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
        return query ? contentItems.filter((item) => matches(item, query)) : contentItems;
    });

    function requestTreeMenu(item: DiskTreeItem, x: number, y: number): void {
        treeMenu = {
            item,
            left: Math.max(8, Math.min(x, window.innerWidth - 228)),
            top: Math.max(8, Math.min(y, window.innerHeight - 244)),
        };
    }

    function chooseTreeAction(action: ImageTreeAction): void {
        if (!treeMenu) return;
        const item = treeMenu.item;
        treeMenu = null;
        onimageaction(item, action);
    }

    function closeMenus(): void {
        imageMenuOpen = false;
        treeMenu = null;
    }
</script>

<svelte:window onclick={closeMenus} onkeydown={(event) => event.key === 'Escape' && closeMenus()} />

<aside class="image-navigator" aria-label="Image navigator">
    <section class="image-summary" aria-label="Active image">
        <header class="image-summary-heading">
            <p class="eyebrow">Image</p>
            {#if storageLocationsAvailable}
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Image options"
                    aria-expanded={imageMenuOpen}
                    title="Image options"
                    disabled={opening}
                    onclick={(event) => {
                        event.stopPropagation();
                        imageMenuOpen = !imageMenuOpen;
                    }}
                >
                    <Icon name="more" size={15} />
                </button>
            {/if}
        </header>

        {#if image}
            <div class="active-image" role="group" aria-label={`Current image: ${imageName}`} aria-busy={opening}>
                <Icon name="hard-drive" size={16} />
                <span class="active-image-copy">
                    <strong title={image.displayName}>{imageName}</strong>
                    <small title={imageLocation}
                        >{opening ? 'Opening image' : imageLocation || 'Storage location'}</small
                    >
                </span>
                <div class="active-image-actions">
                    <button
                        class="icon-button"
                        type="button"
                        aria-label="Open another image"
                        title="Open another image"
                        disabled={opening}
                        onclick={onopen}
                    >
                        <Icon name="folder-open" size={15} />
                    </button>
                    <button
                        class="icon-button"
                        type="button"
                        aria-label="Eject image"
                        title="Eject image"
                        disabled={opening}
                        onclick={onclose}
                    >
                        <Icon name="eject" size={15} />
                    </button>
                </div>
            </div>
        {:else}
            <div class="image-empty-state">
                <div class="image-empty-actions">
                    <button class="primary-button" type="button" disabled={opening} onclick={onopen}>
                        <Icon name="folder-open" size={14} /> Open image
                    </button>
                    <button class="secondary-button" type="button" disabled={opening} onclick={oncreate}>
                        <Icon name="file-plus" size={14} /> Create image
                    </button>
                </div>
            </div>
        {/if}

        {#if imageMenuOpen}
            <div
                class="image-options-menu"
                role="menu"
                tabindex="-1"
                onclick={(event) => event.stopPropagation()}
                onkeydown={(event) => event.stopPropagation()}
            >
                {#if image}
                    <button
                        type="button"
                        role="menuitem"
                        onclick={() => {
                            imageMenuOpen = false;
                            oncreate();
                        }}
                    >
                        <Icon name="file-plus" size={14} /> Create new image
                    </button>
                {/if}
                <button
                    type="button"
                    role="menuitem"
                    onclick={() => {
                        imageMenuOpen = false;
                        onmanagelocations();
                    }}
                >
                    <Icon name="settings" size={14} /> Storage locations
                </button>
            </div>
        {/if}
    </section>

    <section class="image-contents" aria-label="Image contents">
        <div class="panel-heading">
            <div>
                <p class="eyebrow">Contents</p>
                <h2>Partitions, volumes and objects</h2>
            </div>
        </div>

        {#if image}
            <label class="search-field mx-3 mb-2">
                <Icon name="search" size={15} />
                <input bind:value={filter} type="search" placeholder="Search" aria-label="Search image contents" />
            </label>
        {/if}

        <div
            class="image-tree-scroll min-h-0 flex-1 overflow-y-auto px-2 pb-4"
            class:context-menu-open={treeMenu !== null}
        >
            {#if image}
                {#each visibleItems as item (item.id)}
                    <TreeNode
                        {item}
                        {selectedId}
                        {onselect}
                        {onloadchildren}
                        {volumeActionsEnabled}
                        {partitionActionsEnabled}
                        {packageImportEnabled}
                        {packageExportEnabled}
                        {volumePackageExportEnabled}
                        {volumeFloppyExportEnabled}
                        {audioExportEnabled}
                        {mediaConversionEnabled}
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
    <div
        class="tree-context-menu"
        role="menu"
        aria-label={`${treeMenu.item.name} actions`}
        tabindex="-1"
        style={`left: ${treeMenu.left}px; top: ${treeMenu.top}px;`}
        onclick={(event) => event.stopPropagation()}
        onkeydown={(event) => event.stopPropagation()}
    >
        {#if treeMenu.item.kind === 'partition'}
            {#if packageImportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('import-packages')}
                    >Import packages…</button
                >
            {/if}
            {#if volumePackageExportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-volume-packages')}
                    >Export volume packages…</button
                >
            {/if}
            {#if volumeFloppyExportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-volume-floppies')}
                    >Export volumes to floppies…</button
                >
            {/if}
            {#if mediaConversionEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-cdrom')}
                    >Export CD-ROM image…</button
                >
                {#if partitionActionsEnabled || volumeActionsEnabled}
                    <div class="context-menu-separator" role="separator"></div>
                {/if}
            {/if}
            {#if (packageImportEnabled || volumePackageExportEnabled || volumeFloppyExportEnabled) && !mediaConversionEnabled && partitionActionsEnabled}
                <div class="context-menu-separator" role="separator"></div>
            {/if}
            {#if partitionActionsEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('repair-placement')}
                    >Repair object placement…</button
                >
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('rename-partition')}
                    >Rename partition</button
                >
            {/if}
            {#if volumeActionsEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('add-volume')}>Add volume</button>
            {/if}
        {:else}
            {#if packageImportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('import-package')}
                    >Import package…</button
                >
            {/if}
            {#if packageExportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-package')}
                    >Export package…</button
                >
            {/if}
            {#if audioExportEnabled}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-sfz')}>Export SFZ…</button
                >
            {/if}
            {#if mediaConversionEnabled && treeMenu.item.volumeDirectoryId !== undefined}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('export-floppy')}
                    >Export floppy image…</button
                >
            {/if}
            {#if volumeActionsEnabled}
                {#if packageImportEnabled || packageExportEnabled || audioExportEnabled || mediaConversionEnabled}
                    <div class="context-menu-separator" role="separator"></div>
                {/if}
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('repair-placement')}
                    >Repair object placement…</button
                >
                <button type="button" role="menuitem" onclick={() => chooseTreeAction('rename-volume')}
                    >Rename volume</button
                >
                <button
                    class="danger-menu-item"
                    type="button"
                    role="menuitem"
                    onclick={() => chooseTreeAction('delete-volume')}>Delete volume</button
                >
            {/if}
        {/if}
    </div>
{/if}
