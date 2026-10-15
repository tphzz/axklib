<script lang="ts">
    import type { Snippet } from 'svelte';
    import type { HTMLButtonAttributes } from 'svelte/elements';

    let {
        active = false,
        selected = false,
        children,
        class: className = 'program-row',
        ...attributes
    }: HTMLButtonAttributes & { active?: boolean; selected?: boolean; children: Snippet } = $props();
</script>

<button {...attributes} type="button" class={className} class:active class:selected aria-pressed={selected}>
    {@render children()}
</button>
