<script lang="ts">
    import type { Snippet } from 'svelte';
    let {
        ticks,
        children,
        footer,
        label,
    }: { ticks: (number | string)[]; children: Snippet; footer: Snippet; label: string } = $props();
</script>

<div
    class="graph-frame"
    role="group"
    aria-label={label}
    style:--axis-width={`${Math.max(30, ...ticks.map((tick) => String(tick).length * 6 + 8))}px`}
>
    <div class="axis-y" aria-hidden="true">
        {#each ticks as tick}<span>{tick}</span>{/each}
    </div>
    <div class="graph-surface">{@render children()}</div>
    <div class="graph-axis">{@render footer()}</div>
</div>

<style>
    .graph-frame {
        display: grid;
        grid-template-columns: var(--axis-width) minmax(0, 1fr);
        grid-template-rows: minmax(50px, 1fr) auto;
        min-height: 73px;
        min-width: 0;
        flex: 1;
    }
    .axis-y {
        display: flex;
        flex-direction: column;
        justify-content: space-between;
        padding: 6px 5px 6px 0;
        font: 10px var(--font-mono, monospace);
        text-align: right;
        color: var(--color-text-muted);
        white-space: nowrap;
    }
    .graph-surface {
        position: relative;
        display: flex;
        min-width: 0;
        min-height: 0;
        background: var(--color-panel-deep);
        overflow: hidden;
    }
    .graph-axis {
        grid-column: 2;
        min-width: 0;
        margin: 3px 10px 0;
    }
</style>
