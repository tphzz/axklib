<script lang="ts">
    import { sampleTabs } from './fields';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    let { document, panelId }: { document: ObjectEditorDocument; panelId: string } = $props();
    function select(id: string) {
        document.tab = id;
        document.page = '';
    }
</script>

<div role="tablist" aria-label="Sample parameter tabs" class="sample-tabs">
    {#each sampleTabs as tab, index}
        <button
            role="tab"
            id={`${panelId}-${tab.id}`}
            aria-controls={panelId}
            aria-selected={document.tab === tab.id}
            tabindex={document.tab === tab.id ? 0 : -1}
            onclick={() => select(tab.id)}
            onkeydown={(event) => {
                let next = index;
                if (event.key === 'ArrowRight') next = (index + 1) % sampleTabs.length;
                else if (event.key === 'ArrowLeft') next = (index - 1 + sampleTabs.length) % sampleTabs.length;
                else if (event.key === 'Home') next = 0;
                else if (event.key === 'End') next = sampleTabs.length - 1;
                else return;
                event.preventDefault();
                select(sampleTabs[next]!.id);
                (event.currentTarget.parentElement?.children[next] as HTMLButtonElement)?.focus();
            }}>{tab.label}</button
        >
    {/each}
</div>

<style>
    .sample-tabs {
        display: flex;
        min-width: 0;
        flex: 1;
        height: 36px;
        overflow-x: auto;
        scrollbar-width: none;
    }
    button {
        flex: 0 0 auto;
        padding: 0 10px;
        border: 0;
        border-bottom: 2px solid transparent;
        background: transparent;
        color: var(--color-text-muted);
        font-size: 11px;
        white-space: nowrap;
    }
    button[aria-selected='true'] {
        color: var(--color-text);
        border-bottom-color: var(--color-accent);
        background: var(--color-panel-raised);
    }
    :global([data-editor-under~='700']) button {
        padding-inline: 8px;
    }
</style>
