<script lang="ts">
    import type { DiskTreeItem } from '../types';
    import Icon from './Icon.svelte';
    import TreeNode from './TreeNode.svelte';

    interface Props {
        item: DiskTreeItem;
        selectedId: string;
        depth?: number;
        onselect: (item: DiskTreeItem) => void;
        onloadchildren: (
            parentId: string,
            offset: number,
            limit: number,
        ) => Promise<{ items: DiskTreeItem[]; totalCount: number }>;
        volumeActionsEnabled?: boolean;
        partitionActionsEnabled?: boolean;
        onrequestmenu?: (item: DiskTreeItem, x: number, y: number) => void;
    }

    let {
        item,
        selectedId,
        depth = 0,
        onselect,
        onloadchildren,
        volumeActionsEnabled = false,
        partitionActionsEnabled = false,
        onrequestmenu = () => undefined,
    }: Props = $props();
    let expanded = $state(false);
    let children = $state<DiskTreeItem[]>([]);
    let totalCount = $state(0);
    let loading = $state(false);
    let initialized = false;
    const hasChildren = $derived(item.kind !== 'volume' && totalCount > 0);
    const metadata = $derived(
        item.kind === 'partition' && item.partitionIndex !== undefined
            ? `[Partition ${item.partitionIndex}]`
            : item.kind === 'volume'
              ? '[Volume]'
              : '',
    );

    function containsSelected(nodes: DiskTreeItem[]): boolean {
        return nodes.some((node) => node.id === selectedId || containsSelected(node.children ?? []));
    }

    $effect(() => {
        if (initialized) return;
        expanded = item.kind === 'disk' || item.id === selectedId || containsSelected(item.children ?? []);
        children = item.children ?? [];
        totalCount = item.childCount;
        initialized = true;
        if (expanded && children.length === 0 && hasChildren) void loadMore();
    });

    async function loadMore(): Promise<void> {
        if (loading || children.length >= totalCount) return;
        loading = true;
        try {
            const page = await onloadchildren(item.id, children.length, 64);
            children = [...children, ...page.items];
            totalCount = page.totalCount;
        } finally {
            loading = false;
        }
    }

    async function toggle(): Promise<void> {
        if (!hasChildren) return;
        expanded = !expanded;
        if (expanded && children.length === 0) await loadMore();
    }

    function canOpenMenu(): boolean {
        return (
            item.partitionIndex !== undefined &&
            ((item.kind === 'partition' && (volumeActionsEnabled || partitionActionsEnabled)) ||
                (item.kind === 'volume' && volumeActionsEnabled))
        );
    }

    function openContextMenu(event: MouseEvent): void {
        if (!canOpenMenu()) return;
        event.preventDefault();
        event.stopPropagation();
        onrequestmenu(item, event.clientX, event.clientY);
    }

    function openKeyboardMenu(event: KeyboardEvent): void {
        if (!canOpenMenu() || (event.key !== 'ContextMenu' && !(event.shiftKey && event.key === 'F10'))) return;
        event.preventDefault();
        const target = event.currentTarget as HTMLElement;
        const bounds = target.getBoundingClientRect();
        onrequestmenu(item, bounds.left + Math.min(bounds.width, 180), bounds.bottom);
    }
</script>

<div>
    <div
        class:selected={selectedId === item.id}
        class="tree-row group"
        style:--tree-depth={depth}
        data-audio-drop-volume={volumeActionsEnabled && item.kind === 'volume' ? item.name : undefined}
        data-audio-drop-partition={volumeActionsEnabled && item.kind === 'volume' ? item.partitionIndex : undefined}
    >
        {#if hasChildren}
            <button
                class:expanded
                class="tree-chevron"
                type="button"
                aria-label={`${expanded ? 'Collapse' : 'Expand'} ${item.name}`}
                aria-expanded={expanded}
                onclick={() => void toggle()}
            >
                <Icon name="chevron" size={12} strokeWidth={2} />
            </button>
        {:else}
            <span class="tree-chevron" aria-hidden="true"></span>
        {/if}
        <button
            class="tree-item-select"
            type="button"
            onclick={() => onselect(item)}
            onkeydown={openKeyboardMenu}
            oncontextmenu={openContextMenu}
        >
            <span class:volume={item.kind === 'volume'} class="tree-icon"
                ><Icon
                    name={item.kind === 'disk' ? 'disc' : item.kind === 'object' ? 'waveform' : 'folder'}
                    size={14}
                /></span
            >
            <span class="tree-item-name">{item.name}</span>
            {#if metadata}<span class="tree-item-metadata">{metadata}</span>{/if}
            {#if item.kind === 'disk'}<span class="size-1.5 rounded-full bg-emerald-400 shadow-[0_0_8px_#34d399]"
                ></span>{/if}
        </button>
    </div>

    {#if expanded && hasChildren}
        <div class="tree-children">
            {#each children as child (child.id)}
                <TreeNode
                    item={child}
                    {selectedId}
                    depth={depth + 1}
                    {onselect}
                    {onloadchildren}
                    {volumeActionsEnabled}
                    {partitionActionsEnabled}
                    {onrequestmenu}
                />
            {/each}
            {#if children.length < totalCount}
                <button
                    class="tree-row"
                    style:--tree-depth={depth + 1}
                    type="button"
                    disabled={loading}
                    onclick={() => void loadMore()}
                >
                    <span class="tree-chevron invisible"></span>
                    <span>{loading ? 'Loading' : `Load more (${totalCount - children.length})`}</span>
                </button>
            {/if}
        </div>
    {/if}
</div>
