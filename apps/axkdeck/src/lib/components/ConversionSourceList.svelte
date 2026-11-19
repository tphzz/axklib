<script lang="ts">
    import type { ConversionSourceObject } from '../conversions';
    import ImportReportPagination from './ImportReportPagination.svelte';
    import Icon from './Icon.svelte';
    let {
        objects,
        selected,
        onchange,
        disabled = false,
        bulkLabel = 'All sources in disk set',
        locationLabel = 'Disk / occurrence',
    }: {
        objects: ConversionSourceObject[];
        selected: string[] | undefined;
        onchange: (ids: string[] | undefined) => void;
        disabled?: boolean;
        bulkLabel?: string;
        locationLabel?: string;
    } = $props();
    let page = $state(0);
    const size = 100;
    const roots = $derived(objects.filter((o) => o.selectable));
    const ids = $derived(new Set(selected ?? roots.map((o) => o.id)));
    const index = $derived(new Map(objects.map((o) => [o.id, o])));
    const current = $derived(Math.min(page, Math.max(0, Math.ceil(roots.length / size) - 1)));
    const visible = $derived(roots.slice(current * size, (current + 1) * size));
    function dependencies(root: ConversionSourceObject) {
        const seen = new Set<string>();
        const pending = [...root.dependencies];
        const result: ConversionSourceObject[] = [];
        for (let i = 0; i < pending.length; i++) {
            const id = pending[i];
            if (seen.has(id)) continue;
            seen.add(id);
            const item = index.get(id);
            if (item) {
                result.push(item);
                pending.push(...item.dependencies);
            }
        }
        return result;
    }
    function toggle(id: string) {
        const next = new Set(ids);
        if (!next.delete(id)) next.add(id);
        onchange([...next]);
    }
</script>

<label class="bulk"
    ><input
        type="checkbox"
        class="dialog-checkbox"
        checked={roots.length > 0 && ids.size === roots.length}
        indeterminate={ids.size > 0 && ids.size < roots.length}
        disabled={disabled || !roots.length}
        onchange={() => onchange(ids.size === roots.length ? [] : undefined)}
    />
    {bulkLabel} <small>{ids.size} of {roots.length} selected</small></label
>
<div class="row headings"><span>Source object</span><span>Type</span><span>{locationLabel}</span></div>
{#each visible as object (object.id)}
    <div class="row">
        <label
            ><input
                type="checkbox"
                class="dialog-checkbox"
                checked={ids.has(object.id)}
                {disabled}
                aria-label={`${object.name} · ${object.location}`}
                onchange={() => toggle(object.id)}
            />{object.name}</label
        >
        <span>{object.kind}</span><small>{object.location}</small>
    </div>
{/each}
{#each visible.filter((o) => ids.has(o.id) && o.dependencies.length) as root (root.id)}
    <details open={ids.size === 1}>
        <summary>Required by {root.name}</summary>
        {#each dependencies(root) as item (item.id)}
            <div class="row">
                <span><Icon name="lock" size={12} /> {item.name}</span><span>{item.kind}</span><small
                    >{item.location}</small
                >
            </div>
        {/each}
    </details>
{/each}
{#if roots.length > size}<ImportReportPagination
        count={roots.length}
        page={current}
        label="sources"
        onchange={(value) => (page = value)}
    />{/if}

<style>
    label,
    .row > span:first-child {
        display: flex;
        align-items: center;
        gap: 6px;
    }
    .bulk {
        min-height: 28px;
        padding: 0 6px;
    }
    .bulk small {
        margin-left: auto;
    }
    .row {
        display: grid;
        grid-template-columns: minmax(0, 1.4fr) minmax(66px, 0.55fr) minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        min-height: 28px;
        padding: 3px 6px;
        overflow-wrap: anywhere;
    }
    .row > span:nth-child(2) {
        text-transform: capitalize;
    }
    .headings {
        background: var(--color-panel-raised);
        min-height: 24px;
    }
    small {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    details {
        border-top: 1px solid var(--color-border);
    }
    summary {
        padding: 5px 6px;
        cursor: pointer;
    }
</style>
