<script lang="ts" generics="T">
    import type { Snippet } from 'svelte';
    let {
        groups,
        field,
        selected = $bindable(),
    }: { groups: { title: string; fields: T[] }[]; field: Snippet<[T]>; selected?: string } = $props();
    const active = $derived(groups.some((group) => group.title === selected) ? selected : groups[0]?.title);
</script>

<div class="parameter-group-switch" role="group" aria-label="Parameter groups">
    {#each groups as group}<button
            class="editor-choice"
            aria-pressed={active === group.title}
            onclick={() => (selected = group.title)}>{group.title}</button
        >{/each}
</div>
<div class="parameter-sections">
    {#each groups as group}<section class:active={active === group.title}>
            <h3 class="editor-heading">{group.title}</h3>
            <div class="group-fields">
                {#each group.fields as item}{@render field(item)}{/each}
            </div>
        </section>{/each}
</div>

<style>
    .parameter-group-switch {
        display: none;
        min-width: 0;
        margin-bottom: 6px;
        gap: 2px;
    }
    .parameter-sections {
        display: grid;
        align-content: start;
        gap: 12px;
    }
    h3 {
        margin: 0 0 6px;
    }
    .group-fields {
        display: grid;
        grid-template-columns: minmax(0, 1fr);
        gap: 6px;
    }
    :global([data-controls-over~='650']) .group-fields {
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 6px 16px;
    }
    :global([data-graph-under~='900']) .parameter-group-switch {
        display: flex;
    }
    :global([data-graph-under~='900']) section:not(.active) {
        display: none;
    }
    :global([data-graph-under~='900']) h3 {
        display: none;
    }
</style>
