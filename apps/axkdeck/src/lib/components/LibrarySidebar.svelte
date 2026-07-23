<script lang="ts">
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import Icon from './Icon.svelte';
    import TreeNode from './TreeNode.svelte';

    interface Props {
        items: DiskTreeItem[];
        selectedId: string;
        onselect: (item: DiskTreeItem) => void;
        onloadchildren: (
            parentId: string,
            offset: number,
            limit: number,
        ) => Promise<{ items: DiskTreeItem[]; totalCount: number }>;
        volumeActionsEnabled: boolean;
        partitionActionsEnabled: boolean;
        onimageaction: (item: DiskTreeItem, action: ImageTreeAction) => void;
    }

    let {
        items,
        selectedId,
        onselect,
        onloadchildren,
        volumeActionsEnabled,
        partitionActionsEnabled,
        onimageaction,
    }: Props = $props();
    let filter = $state('');
    let menu = $state<{ item: DiskTreeItem; left: number; top: number } | null>(null);

    function matches(item: DiskTreeItem, query: string): boolean {
        return (
            item.name.toLocaleLowerCase().includes(query) ||
            Boolean(item.children?.some((child) => matches(child, query)))
        );
    }

    const visibleItems = $derived.by(() => {
        const query = filter.trim().toLocaleLowerCase();
        return query ? items.filter((item) => matches(item, query)) : items;
    });

    function requestMenu(item: DiskTreeItem, x: number, y: number): void {
        menu = {
            item,
            left: Math.max(8, Math.min(x, window.innerWidth - 180)),
            top: Math.max(8, Math.min(y, window.innerHeight - 112)),
        };
    }

    function chooseAction(action: ImageTreeAction): void {
        if (!menu) return;
        const item = menu.item;
        menu = null;
        onimageaction(item, action);
    }
</script>

<svelte:window onclick={() => (menu = null)} onkeydown={(event) => event.key === 'Escape' && (menu = null)} />

<aside class="library-sidebar" aria-label="Volume browser">
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Disk images</p>
            <h2>Volumes</h2>
        </div>
    </div>

    <label class="search-field mx-3 mb-2">
        <Icon name="search" size={15} />
        <input bind:value={filter} type="search" placeholder="Search" aria-label="Search volumes" />
    </label>

    <div class="min-h-0 flex-1 overflow-y-auto px-2 pb-4">
        {#each visibleItems as item (item.id)}
            <TreeNode
                {item}
                {selectedId}
                {onselect}
                {onloadchildren}
                {volumeActionsEnabled}
                {partitionActionsEnabled}
                onrequestmenu={requestMenu}
            />
        {:else}
            <p class="empty-copy">No matching volumes</p>
        {/each}
    </div>
</aside>

{#if menu}
    <div
        class="tree-context-menu"
        role="menu"
        aria-label={`${menu.item.name} actions`}
        tabindex="-1"
        style={`left: ${menu.left}px; top: ${menu.top}px;`}
        onclick={(event) => event.stopPropagation()}
        onkeydown={(event) => event.stopPropagation()}
    >
        {#if menu.item.kind === 'partition'}
            {#if partitionActionsEnabled}
                <button type="button" role="menuitem" onclick={() => chooseAction('rename-partition')}
                    >Rename partition</button
                >
            {/if}
            {#if volumeActionsEnabled}
                <button type="button" role="menuitem" onclick={() => chooseAction('add-volume')}>Add volume</button>
            {/if}
        {:else}
            <button type="button" role="menuitem" onclick={() => chooseAction('rename-volume')}>Rename volume</button>
            <button class="danger-menu-item" type="button" role="menuitem" onclick={() => chooseAction('delete-volume')}
                >Delete volume</button
            >
        {/if}
    </div>
{/if}
