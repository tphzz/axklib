<script lang="ts">
    import type { Snippet } from 'svelte';

    let {
        children,
        status,
        tools,
        context = 'editor',
    }: {
        children: Snippet;
        status?: Snippet;
        tools?: Snippet;
        context?: 'editor' | 'dialog';
    } = $props();
    const single = $derived(context === 'dialog' && !status && !tools);
</script>

<svelte:element this={context === 'dialog' ? 'div' : 'header'} class="tab-bar" class:single data-context={context}>
    {@render children()}
    {#if !single}<div class="header-status">{@render status?.()}</div>{/if}
    {#if tools}<div class="header-tools">{@render tools()}</div>{/if}
</svelte:element>

<style>
    .tab-bar {
        display: grid;
        grid-template-columns: minmax(0, max-content) minmax(0, 1fr) auto;
        align-items: center;
        gap: 8px;
        height: var(--density-section-toolbar);
        flex: 0 0 var(--density-section-toolbar);
        min-width: 0;
        padding: 0 8px;
        box-shadow: inset 0 -1px var(--color-border);
        background: var(--color-panel);
    }
    .tab-bar[data-context='dialog'] {
        padding: 0 var(--density-section-gap);
    }
    .single {
        grid-template-columns: minmax(0, 1fr);
        gap: 0;
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
