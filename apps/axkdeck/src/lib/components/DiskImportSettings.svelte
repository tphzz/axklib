<script lang="ts">
    import type { Snippet } from 'svelte';
    let { children }: { children: Snippet } = $props();
</script>

<div class="disk-import-settings">{@render children()}</div>

<style>
    .disk-import-settings {
        display: flex;
        flex: none;
        flex-wrap: wrap;
        align-items: center;
        gap: 4px;
        padding: 6px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        min-width: 0;
    }
</style>
