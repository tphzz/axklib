<script lang="ts">
    import { tick } from 'svelte';
    import { formatAllocationBytes } from '../allocationInspector';
    import { hasDisallowedNavigationModifier, linearNavigationIndex } from '../collectionNavigation';
    import { formatStoredSize } from '../formatBytes';
    import { orderSamplerTreeItems } from '../samplerTreeOrder';
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
        samplerOrderingEnabled?: boolean;
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
        samplerOrderingEnabled = false,
        onrequestmenu = () => undefined,
    }: Props = $props();
    let expanded = $state(false);
    let children = $state<DiskTreeItem[]>([]);
    let totalCount = $state(0);
    let loading = $state(false);
    let loadError = $state('');
    let initialized = false;
    const hasChildren = $derived(item.kind !== 'volume' && totalCount > 0);
    const loadCompletePartition = $derived(samplerOrderingEnabled && item.kind === 'partition');
    const orderedChildren = $derived(loadCompletePartition ? orderSamplerTreeItems(children, 'volume') : children);
    const metadata = $derived(
        item.kind === 'partition' && item.partitionIndex !== undefined
            ? `[Partition ${item.partitionIndex}]`
            : item.kind === 'volume'
              ? '[Volume]'
              : '',
    );
    const partitionCapacity = $derived(item.kind === 'partition' ? item.partitionCapacity : undefined);
    const usableClusters = $derived(
        partitionCapacity ? partitionCapacity.allocatedClusters + partitionCapacity.freeClusters : 0,
    );
    const usedPercent = $derived(
        partitionCapacity && usableClusters > 0
            ? Math.min(100, Math.round((partitionCapacity.allocatedClusters / usableClusters) * 100))
            : 0,
    );
    const capacityLevel = $derived(usedPercent >= 90 ? 'critical' : usedPercent >= 80 ? 'warning' : 'normal');
    const partitionTooltipId = $derived(`partition-capacity-${item.id}`);
    const volumeTooltipId = $derived(`volume-size-${item.id}`);
    const tooltipId = $derived(
        loadCompletePartition
            ? partitionTooltipId
            : item.kind === 'volume' && item.sizeBytes !== undefined
              ? volumeTooltipId
              : undefined,
    );
    const partitionVolumeText = $derived(`${item.childCount} ${item.childCount === 1 ? 'volume' : 'volumes'}`);
    const partitionCapacityText = $derived(
        partitionCapacity
            ? `${partitionCapacity.allocatedClusters.toLocaleString()} of ${usableClusters.toLocaleString()} clusters used`
            : 'Capacity unavailable',
    );
    const partitionSpaceText = $derived(
        partitionCapacity
            ? `${formatAllocationBytes(partitionCapacity.allocatedClusters * partitionCapacity.clusterSizeBytes)} of ${formatAllocationBytes(usableClusters * partitionCapacity.clusterSizeBytes)} used`
            : '',
    );

    function containsSelected(nodes: DiskTreeItem[]): boolean {
        return nodes.some((node) => node.id === selectedId || containsSelected(node.children ?? []));
    }

    $effect(() => {
        if (initialized) return;
        expanded =
            item.kind === 'disk' ||
            loadCompletePartition ||
            item.id === selectedId ||
            containsSelected(item.children ?? []);
        children = item.children ?? [];
        totalCount = item.childCount;
        initialized = true;
        if (expanded && children.length < totalCount && (children.length === 0 || loadCompletePartition)) {
            void loadMore();
        }
    });

    async function loadMore(): Promise<void> {
        if (loading || children.length >= totalCount) return;
        loading = true;
        loadError = '';
        try {
            const loaded = [...children];
            let loadedTotalCount = totalCount;
            do {
                const page = await onloadchildren(item.id, loaded.length, 64);
                if (!Number.isSafeInteger(page.totalCount) || page.totalCount < loaded.length + page.items.length) {
                    throw new Error('The server returned an invalid tree page');
                }
                if (page.items.length === 0 && loaded.length < page.totalCount) {
                    throw new Error('The server returned an incomplete tree page');
                }
                loaded.push(...page.items);
                loadedTotalCount = page.totalCount;
            } while (loadCompletePartition && loaded.length < loadedTotalCount);
            children = loaded;
            totalCount = loadedTotalCount;
        } catch (reason) {
            loadError = reason instanceof Error ? reason.message : String(reason);
        } finally {
            loading = false;
        }
    }

    async function toggle(): Promise<void> {
        if (!hasChildren) return;
        expanded = !expanded;
        if (expanded && children.length < totalCount && (children.length === 0 || loadCompletePartition)) {
            await loadMore();
        }
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
        class:partition-summary-row={loadCompletePartition}
        class="tree-row group"
        style:--tree-depth={depth}
        data-import-drop-volume={item.kind === 'volume' ? item.name : undefined}
        data-import-drop-partition={item.kind === 'volume' || item.kind === 'partition'
            ? item.partitionIndex
            : undefined}
        data-import-drop-kind={item.kind === 'volume' || item.kind === 'partition' ? item.kind : undefined}
        data-import-drop-name={item.kind === 'volume' || item.kind === 'partition' ? item.name : undefined}
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
        <div class="tree-item-stack">
            <button
                class="tree-item-select"
                type="button"
                aria-current={selectedId === item.id ? 'true' : undefined}
                aria-describedby={tooltipId}
                data-tree-id={item.id}
                data-tree-parent-id={parentId}
                onclick={() => onselect(item)}
                ondblclick={() => {
                    if (item.kind === 'partition') void toggle();
                }}
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
            {#if loadCompletePartition}
                {#if partitionCapacity}
                    <span
                        class:warning={capacityLevel === 'warning'}
                        class:critical={capacityLevel === 'critical'}
                        class="partition-capacity"
                        role="progressbar"
                        aria-label={`${usedPercent}% used`}
                        aria-valuemin="0"
                        aria-valuemax="100"
                        aria-valuenow={usedPercent}
                    >
                        <span class="partition-capacity-fill" style:width={`${usedPercent}%`}></span>
                    </span>
                {:else}
                    <span class="partition-capacity unavailable" aria-hidden="true"></span>
                {/if}
                <span id={partitionTooltipId} class="tree-item-tooltip" role="tooltip">
                    <span>{partitionVolumeText}</span>
                    <span>{partitionCapacityText}</span>
                    {#if partitionSpaceText}<span>{partitionSpaceText}</span>{/if}
                </span>
            {:else if item.kind === 'volume' && item.sizeBytes !== undefined}
                <span id={volumeTooltipId} class="tree-item-tooltip" role="tooltip">
                    <span>Size: {formatStoredSize(item.sizeBytes)}</span>
                </span>
            {/if}
        </div>
    </div>

    {#if expanded && hasChildren}
        <div class="tree-children">
            {#if loadError}
                <div class="tree-load-error" role="alert">
                    <span>{loadError}</span>
                    <button type="button" onclick={() => void loadMore()}>Retry</button>
                </div>
            {/if}
            {#each orderedChildren as child (child.id)}
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
                    {samplerOrderingEnabled}
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

<style>
    .tree-row {
        position: relative;
    }

    .partition-summary-row {
        height: 27px;
    }

    .partition-summary-row > .tree-chevron {
        align-self: flex-start;
        margin-top: 4px;
    }

    .tree-item-stack {
        position: relative;
        min-width: 0;
        height: 100%;
        flex: 1;
    }

    .tree-item-stack > .tree-item-select {
        width: 100%;
        height: 100%;
    }

    .partition-summary-row .tree-item-select {
        box-sizing: border-box;
        padding-bottom: 6px;
    }

    .partition-capacity {
        position: absolute;
        left: 0;
        right: 0;
        bottom: 1px;
        height: 3px;
        overflow: hidden;
        border-radius: 1px;
        background: var(--color-border);
        pointer-events: none;
    }

    .partition-capacity.unavailable {
        border-bottom: 1px dashed var(--color-border);
        background: transparent;
        opacity: 0.55;
    }

    .partition-capacity-fill {
        display: block;
        height: 100%;
        background: var(--color-accent);
    }

    .partition-capacity.warning .partition-capacity-fill {
        background: var(--color-brand);
    }

    .partition-capacity.critical .partition-capacity-fill {
        background: var(--color-danger);
    }

    .tree-item-tooltip {
        position: absolute;
        z-index: 70;
        top: calc(100% + 3px);
        right: 4px;
        display: grid;
        width: max-content;
        max-width: 230px;
        gap: 2px;
        padding: 6px 8px;
        color: var(--color-text);
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: var(--color-panel-raised);
        box-shadow: 0 8px 20px rgb(0 0 0 / 35%);
        opacity: 0;
        pointer-events: none;
        transform: translateY(-2px);
        transition:
            opacity 100ms ease,
            transform 100ms ease;
    }

    .tree-row:hover > .tree-item-stack > .tree-item-tooltip,
    .tree-item-select:focus-visible ~ .tree-item-tooltip {
        opacity: 1;
        transform: translateY(0);
    }
</style>
