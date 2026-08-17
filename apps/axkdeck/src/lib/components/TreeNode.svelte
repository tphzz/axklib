<script lang="ts">
    import { tick } from 'svelte';
    import { hasDisallowedNavigationModifier, linearNavigationIndex } from '../collectionNavigation';
    import type { DiskTreeItem } from '../types';
    import Icon from './Icon.svelte';
    import TreeNode from './TreeNode.svelte';

    interface Props {
        item: DiskTreeItem;
        selectedId: string;
        depth?: number;
        parentId?: string;
        onselect: (item: DiskTreeItem) => void;
        onloadchildren: (
            parentId: string,
            offset: number,
            limit: number,
        ) => Promise<{ items: DiskTreeItem[]; totalCount: number }>;
        volumeActionsEnabled?: boolean;
        partitionActionsEnabled?: boolean;
        packageImportEnabled?: boolean;
        packageExportEnabled?: boolean;
        volumePackageExportEnabled?: boolean;
        volumeFloppyExportEnabled?: boolean;
        audioExportEnabled?: boolean;
        mediaConversionEnabled?: boolean;
        allocationInspectionEnabled?: boolean;
        onrequestmenu?: (item: DiskTreeItem, x: number, y: number) => void;
    }

    let {
        item,
        selectedId,
        depth = 0,
        parentId = '',
        onselect,
        onloadchildren,
        volumeActionsEnabled = false,
        partitionActionsEnabled = false,
        packageImportEnabled = false,
        packageExportEnabled = false,
        volumePackageExportEnabled = false,
        volumeFloppyExportEnabled = false,
        audioExportEnabled = false,
        mediaConversionEnabled = false,
        allocationInspectionEnabled = false,
        onrequestmenu = () => undefined,
    }: Props = $props();
    let expanded = $state(false);
    let children = $state<DiskTreeItem[]>([]);
    let totalCount = $state(0);
    let loading = $state(false);
    let loadError = $state('');
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
        loadError = '';
        try {
            const page = await onloadchildren(item.id, children.length, 64);
            if (!Number.isSafeInteger(page.totalCount) || page.totalCount < children.length + page.items.length) {
                throw new Error('The server returned an invalid tree page');
            }
            if (page.items.length === 0 && children.length < page.totalCount) {
                throw new Error('The server returned an incomplete tree page');
            }
            children = [...children, ...page.items];
            totalCount = page.totalCount;
        } catch (reason) {
            loadError = reason instanceof Error ? reason.message : String(reason);
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
            ((item.kind === 'partition' &&
                (volumeActionsEnabled ||
                    partitionActionsEnabled ||
                    volumePackageExportEnabled ||
                    volumeFloppyExportEnabled ||
                    allocationInspectionEnabled ||
                    mediaConversionEnabled)) ||
                (item.kind === 'volume' &&
                    (volumeActionsEnabled ||
                        packageImportEnabled ||
                        packageExportEnabled ||
                        audioExportEnabled ||
                        (mediaConversionEnabled && item.volumeDirectoryId !== undefined))))
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

    function treeButtons(current: HTMLElement): HTMLElement[] {
        const tree = current.closest('.image-tree-scroll') ?? current.ownerDocument;
        return [...tree.querySelectorAll<HTMLElement>('.tree-item-select[data-tree-id]')];
    }

    function selectAndFocus(target: HTMLElement): void {
        target.click();
        target.focus({ preventScroll: true });
        target.scrollIntoView?.({ block: 'nearest' });
    }

    async function handleTreeKeyboard(event: KeyboardEvent): Promise<void> {
        if (hasDisallowedNavigationModifier(event)) {
            openKeyboardMenu(event);
            return;
        }
        const current = event.currentTarget as HTMLElement;
        if (event.key === 'ArrowRight') {
            if (!hasChildren) return;
            event.preventDefault();
            if (!expanded) {
                await toggle();
                return;
            }
            await tick();
            const child = treeButtons(current).find((button) => button.dataset.treeParentId === item.id);
            if (child) selectAndFocus(child);
            return;
        }
        if (event.key === 'ArrowLeft') {
            if (!expanded && !parentId) return;
            event.preventDefault();
            if (expanded) {
                await toggle();
                return;
            }
            const parent = treeButtons(current).find((button) => button.dataset.treeId === parentId);
            if (parent) selectAndFocus(parent);
            return;
        }
        const buttons = treeButtons(current);
        const currentIndex = buttons.indexOf(current);
        const targetIndex = linearNavigationIndex(event.key, currentIndex, buttons.length);
        if (targetIndex !== null) {
            event.preventDefault();
            if (targetIndex === currentIndex) return;
            const target = buttons[targetIndex];
            if (target) selectAndFocus(target);
            return;
        }
        openKeyboardMenu(event);
    }
</script>

<div>
    <div
        class:selected={selectedId === item.id}
        class="tree-row group"
        style:--tree-depth={depth}
        data-import-drop-volume={item.kind === 'volume' ? item.name : undefined}
        data-import-drop-partition={item.kind === 'volume' ? item.partitionIndex : undefined}
    >
        {#if hasChildren}
            <button
                class:expanded
                class="tree-chevron"
                type="button"
                aria-label={`${expanded ? 'Collapse' : 'Expand'} ${item.name}`}
                aria-expanded={expanded}
                tabindex="-1"
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
            aria-current={selectedId === item.id ? 'true' : undefined}
            data-tree-id={item.id}
            data-tree-parent-id={parentId}
            onclick={() => onselect(item)}
            onkeydown={(event) => void handleTreeKeyboard(event)}
            oncontextmenu={openContextMenu}
        >
            <span class:volume={item.kind === 'volume'} class="tree-icon"
                ><Icon
                    name={item.kind === 'disk' ? 'disc' : item.kind === 'object' ? 'waveform' : 'folder'}
                    size={14}
                /></span
            >
            <span class="tree-item-name" style:white-space="pre">{item.name}</span>
            {#if metadata}<span class="tree-item-metadata">{metadata}</span>{/if}
            {#if item.kind === 'disk'}<span class="size-1.5 rounded-full bg-emerald-400 shadow-[0_0_8px_#34d399]"
                ></span>{/if}
        </button>
    </div>

    {#if expanded && hasChildren}
        <div class="tree-children">
            {#if loadError}
                <div class="tree-load-error" role="alert">
                    <span>{loadError}</span>
                    <button type="button" onclick={() => void loadMore()}>Retry</button>
                </div>
            {/if}
            {#each children as child (child.id)}
                <TreeNode
                    item={child}
                    {selectedId}
                    depth={depth + 1}
                    parentId={item.id}
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
                    {allocationInspectionEnabled}
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
