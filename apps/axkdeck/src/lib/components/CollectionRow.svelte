<script lang="ts">
    import type { Snippet } from 'svelte';
    import type { HTMLButtonAttributes } from 'svelte/elements';

    let {
        active = false,
        selected = false,
        slotLabel,
        children,
        ...attributes
    }: Omit<HTMLButtonAttributes, 'class' | 'type' | 'aria-pressed'> & {
        active?: boolean;
        selected?: boolean;
        slotLabel?: string;
        children: Snippet;
    } = $props();
</script>

<button
    {...attributes}
    type="button"
    class="collection-row"
    class:has-slot={slotLabel !== undefined}
    class:active
    class:selected
    aria-pressed={selected}
>
    {#if slotLabel !== undefined}<span class="collection-row-slot">{slotLabel}</span>{/if}
    <span class="collection-row-identity">{@render children()}</span>
</button>

<style>
    .collection-row {
        display: grid;
        box-sizing: border-box;
        width: 100%;
        height: var(--density-row);
        min-width: 0;
        min-height: var(--density-row);
        grid-template-columns: minmax(0, 1fr);
        align-items: center;
        gap: 4px;
        padding: 0;
        overflow: hidden;
        color: var(--color-text);
        border: 1px solid transparent;
        border-bottom-color: var(--color-border);
        background: transparent;
        cursor: pointer;
        text-align: left;
        transition: 130ms ease;
        font: inherit;
    }

    .collection-row.has-slot {
        grid-template-columns: 30px minmax(0, 1fr);
    }

    .collection-row-slot {
        align-self: stretch;
        padding: 3px 6px 1px;
        color: var(--color-text-muted);
        font-family: var(--font-mono);
        font-size: 8.5px;
        line-height: 10px;
    }

    .collection-row:hover {
        border-color: rgb(104 151 187 / 42%);
        border-radius: 5px;
        background: #293036;
    }

    .collection-row.active {
        border-color: rgb(104 151 187 / 68%);
        border-radius: 5px;
    }

    .collection-row.active:not(.selected) {
        background: rgb(104 151 187 / 10%);
    }

    .collection-row.selected {
        border-color: rgb(104 151 187 / 68%);
        border-radius: 5px;
        background: linear-gradient(120deg, rgb(55 91 119 / 68%), rgb(49 60 68 / 68%));
    }

    .collection-row-identity {
        display: flex;
        box-sizing: border-box;
        min-width: 0;
        min-height: calc(var(--density-row) - 2px);
        flex-direction: column;
        justify-content: center;
        padding: 2px 6px;
        overflow: hidden;
        font-size: 10px;
        line-height: 10px;
    }

    .has-slot .collection-row-identity {
        padding-left: 0;
    }
</style>
