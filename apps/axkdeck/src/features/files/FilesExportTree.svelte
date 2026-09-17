<script lang="ts">
    import { tick } from 'svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { formatStoredSize } from '../../lib/formatBytes';
    import type { FileTreePreview } from './exportTree';
    import { buildExportTree, visibleExportRows, exportTreePage, type ExportTreeRow } from './exportTree';
    let {
        inspection,
        name,
        label = 'Export entries',
    }: { inspection: FileTreePreview; name: string; label?: string } = $props();
    const root = $derived(buildExportTree(inspection, name));
    let collapsed = $state<Set<string>>(new Set());
    let page = $state(0);
    let active = $state('[]');
    let scroller: HTMLDivElement;
    const rows = $derived(visibleExportRows(root, collapsed));
    const pages = $derived(Math.max(1, Math.ceil(rows.length / 100)));
    const pageRows = $derived(exportTreePage(rows, Math.min(page, pages - 1)));
    $effect(() => {
        if (page >= pages) page = pages - 1;
        if (!pageRows.some((row) => row.id === active)) active = pageRows[0]?.id ?? '[]';
    });
    async function focus(row: ExportTreeRow) {
        active = row.id;
        const index = rows.findIndex((item) => item.id === row.id);
        if (!pageRows.some((item) => item.id === row.id)) page = Math.floor(index / 100);
        await tick();
        const element = Array.from(scroller.querySelectorAll<HTMLElement>('[data-export-row]')).find(
            (item) => item.dataset.exportRow === row.id,
        );
        element?.focus({ preventScroll: true });
        if (element) {
            const bounds = element.getBoundingClientRect();
            const viewport = scroller.getBoundingClientRect();
            if (bounds.top < viewport.top) scroller.scrollTop -= viewport.top - bounds.top;
            else if (bounds.bottom > viewport.bottom) scroller.scrollTop += bounds.bottom - viewport.bottom;
        }
    }
    async function toggle(row: ExportTreeRow) {
        const next = new Set(collapsed);
        if (next.has(row.id)) next.delete(row.id);
        else next.add(row.id);
        collapsed = next;
        await tick();
        await focus(row);
    }
    function changePage(next: number) {
        page = next;
        scroller.scrollTop = 0;
    }
    function navigate(event: KeyboardEvent, row: ExportTreeRow) {
        const index = rows.indexOf(row);
        let target: ExportTreeRow | undefined;
        switch (event.key) {
            case 'ArrowDown':
                target = rows[index + 1];
                break;
            case 'ArrowUp':
                target = rows[index - 1];
                break;
            case 'Home':
                target = rows[0];
                break;
            case 'End':
                target = rows.at(-1);
                break;
            case 'PageDown':
                target = rows[Math.min(rows.length - 1, index + 100)];
                break;
            case 'PageUp':
                target = rows[Math.max(0, index - 100)];
                break;
            case 'ArrowRight':
                if (collapsed.has(row.id) && row.children.length) void toggle(row);
                else target = row.children[0];
                break;
            case 'ArrowLeft':
                if (!collapsed.has(row.id) && row.children.length) void toggle(row);
                else target = row.parent ?? undefined;
                break;
            case 'Enter':
            case ' ':
                if (row.children.length) void toggle(row);
                break;
            default:
                return;
        }
        event.preventDefault();
        event.stopPropagation();
        if (target) void focus(target);
    }
</script>

<div class="export-tree" role="treegrid" aria-label={label}>
    <div class="export-row export-heading" role="row">
        <span role="columnheader">Name</span><span role="columnheader">Size</span>
    </div>
    <div class="export-rows" role="rowgroup" bind:this={scroller}>
        {#each pageRows as row (row.id)}
            <!-- svelte-ignore a11y_no_noninteractive_tabindex a11y_no_noninteractive_element_interactions -->
            <div
                class="export-row"
                role="row"
                aria-level={row.depth + 1}
                aria-expanded={row.children.length ? !collapsed.has(row.id) : undefined}
                tabindex={active === row.id ? 0 : -1}
                data-export-row={row.id}
                onfocus={() => (active = row.id)}
                onkeydown={(event) => navigate(event, row)}
            >
                <div
                    class="export-name"
                    role="gridcell"
                    title={row.path}
                    style:padding-left={`${Math.min(row.depth, 8) * 16}px`}
                >
                    {#if row.children.length}
                        <button
                            class="icon-button tree-toggle"
                            tabindex="-1"
                            type="button"
                            aria-label={`${collapsed.has(row.id) ? 'Expand' : 'Collapse'} ${row.name}`}
                            onclick={() => void toggle(row)}
                        >
                            <span class:expanded={!collapsed.has(row.id)}><Icon name="chevron" size={12} /></span>
                        </button>
                    {:else}<span class="tree-toggle"></span>{/if}
                    <Icon name={row.directory ? 'folder' : 'file'} size={13} />
                    <span class="export-label">{row.name}</span>
                </div>
                <span role="gridcell">{row.directory ? '' : formatStoredSize(row.sizeBytes)}</span>
            </div>
        {/each}
    </div>
</div>
<nav class="export-pagination" aria-label="Export entry pages" class:single-page={pages === 1}>
    <button
        class="icon-button previous-page"
        type="button"
        aria-label="Previous page"
        title="Previous page"
        disabled={page === 0}
        onclick={() => changePage(page - 1)}><Icon name="chevron" size={14} /></button
    >
    <span>{page + 1} / {pages}</span>
    <button
        class="icon-button"
        type="button"
        aria-label="Next page"
        title="Next page"
        disabled={page === pages - 1}
        onclick={() => changePage(page + 1)}><Icon name="chevron" size={14} /></button
    >
</nav>

<style>
    .export-tree {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }
    .export-row {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 72px;
        align-items: start;
        gap: 8px;
        min-height: 26px;
        padding: 4px 0;
        border-bottom: 1px solid var(--color-border);
        font-size: var(--dialog-body-font-size);
    }
    .export-row > :last-child {
        text-align: right;
    }
    .export-row:focus {
        outline: 1px solid var(--color-accent);
        outline-offset: -1px;
    }
    .export-heading {
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
        overflow-y: hidden;
    }
    .export-heading,
    .export-rows {
        padding-right: calc(8px + var(--overlay-scrollbar-clearance));
        scrollbar-gutter: stable;
    }
    .export-rows {
        overflow: auto;
        flex: 1;
        min-height: 0;
    }
    .export-name {
        display: flex;
        gap: 6px;
        align-items: center;
        min-width: 0;
    }
    .export-label {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .tree-toggle {
        display: inline-flex;
        width: 14px;
        height: 16px;
        min-width: 14px;
        padding: 0;
        flex-shrink: 0;
    }
    .tree-toggle span {
        display: flex;
    }
    .expanded {
        transform: rotate(90deg);
    }
    .export-pagination {
        display: flex;
        justify-content: flex-end;
        align-items: center;
        gap: 8px;
        height: 22px;
        flex-shrink: 0;
        font-size: var(--dialog-metadata-font-size);
    }
    .single-page {
        visibility: hidden;
    }
    .previous-page :global(svg) {
        transform: rotate(180deg);
    }
</style>
