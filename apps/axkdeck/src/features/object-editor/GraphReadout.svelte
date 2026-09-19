<script lang="ts">
    import type { Snippet } from 'svelte';
    let { text = '', title, tools }: { text?: string; title?: Snippet; tools?: Snippet } = $props();
</script>

<div class="graph-readout">
    {#if title}<div class="graph-title">{@render title()}</div>{/if}
    <output title={text}>{text}</output>
    {#if tools}<div class="graph-tools">{@render tools()}</div>{/if}
</div>

<style>
    .graph-readout {
        display: flex;
        align-items: center;
        gap: 6px;
        height: 26px;
        flex: 0 0 26px;
        min-width: 0;
    }
    output {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        color: var(--editor-loop);
        font: 10px var(--font-mono, monospace);
        padding-inline: 4px;
    }
    .graph-tools {
        display: flex;
        flex: 0 0 auto;
        align-items: center;
    }
    .graph-title {
        min-width: 0;
        flex: 0 1 auto;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
</style>
