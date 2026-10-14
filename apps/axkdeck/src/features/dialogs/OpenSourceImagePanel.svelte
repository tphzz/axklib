<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import StorageBrowserPanel from '../../lib/components/StorageBrowserPanel.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import type { ImageLocation } from '../../lib/storageLocations';

    let browser: ComponentProps<typeof StorageBrowserPanel> = $props();
    let sources = $state<ImageLocation[]>([]);
    let error = $state('');
    function key(source: ImageLocation): string {
        return source.kind === 'device'
            ? `${source.reference.sessionId}:${source.reference.targetId}`
            : `${source.reference.rootId}:${source.reference.relativePath}`;
    }
    function add(items: ImageLocation[]): void {
        const next = [...sources];
        for (const item of items) if (!next.some((source) => key(source) === key(item))) next.push(item);
        if (next.length > 32) {
            error = 'A source set can contain at most 32 members.';
            return;
        }
        sources = next;
        error = '';
    }
</script>

<div class="source-browser">
    <StorageBrowserPanel
        {...browser}
        extensions={[]}
        multiple
        selectionAction="add"
        onselect={(item) => add([item])}
        onselectmany={add}
    />
</div>
<section class="source-set" aria-label="Source set">
    <h3>Sources ({sources.length}/32)</h3>
    <ul>
        {#each sources as source (key(source))}
            <li>
                <Icon name={source.kind === 'server-directory' ? 'folder' : 'file'} size={14} />
                <span title={source.displayName}>{source.displayName}</span>
                <button
                    class="icon-button"
                    type="button"
                    title="Remove source"
                    aria-label={`Remove ${source.displayName}`}
                    onclick={() => {
                        sources = sources.filter((item) => item !== source);
                        error = '';
                    }}><Icon name="close" size={14} /></button
                >
            </li>
        {:else}<li class="empty-copy">No sources selected</li>{/each}
    </ul>
</section>
<footer class="dialog-footer">
    <span class="dialog-footer-status" role="status"
        >{error || `${sources.length} ${sources.length === 1 ? 'source' : 'sources'} selected`}</span
    >
    <div class="dialog-footer-actions">
        <button class="secondary-button" type="button" onclick={browser.oncancel}>Cancel</button>
        <button
            class="primary-button"
            type="button"
            disabled={!sources.length}
            onclick={() => browser.onselectsources?.(sources)}>Open</button
        >
    </div>
</footer>

<style>
    .source-browser {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 100px;
        overflow: hidden;
    }
    .source-set {
        border-top: 1px solid var(--color-border);
        padding: 8px 12px;
    }
    h3 {
        font-size: 10px;
        margin: 0 0 4px;
        color: var(--color-text-muted);
    }
    ul {
        height: 80px;
        overflow: auto;
        list-style: none;
        padding: 0;
        margin: 0;
    }
    li {
        display: flex;
        align-items: center;
        gap: 6px;
        height: 25px;
        font-size: 11px;
    }
    li span {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
    }
</style>
