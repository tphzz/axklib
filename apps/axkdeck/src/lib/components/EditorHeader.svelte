<script lang="ts">
    import type { Snippet } from 'svelte';

    let {
        children,
        status,
        tools,
        secondary = false,
    }: {
        children: Snippet;
        status?: Snippet;
        tools?: Snippet;
        secondary?: boolean;
    } = $props();
</script>

<header class="editor-header" class:secondary>
    {@render children()}
    <div class="header-status">{@render status?.()}</div>
    {#if tools}<div class="header-tools">{@render tools()}</div>{/if}
</header>

<style>
    .editor-header {
        display: grid;
        grid-template-columns: minmax(0, max-content) minmax(0, 1fr) auto;
        align-items: center;
        gap: 8px;
        height: var(--density-section-toolbar);
        flex: 0 0 var(--density-section-toolbar);
        min-width: 0;
        padding: 0 8px;
        border-bottom: 1px solid var(--color-border);
        background: var(--color-panel);
    }
    .secondary {
        height: 28px;
        flex-basis: 28px;
    }
    .header-status {
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .header-tools {
        display: flex;
        align-items: center;
        justify-self: end;
        white-space: nowrap;
    }
</style>
