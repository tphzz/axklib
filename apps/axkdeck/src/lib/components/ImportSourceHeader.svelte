<script lang="ts">
    import type { Snippet } from 'svelte';
    import AttributeHelp from './AttributeHelp.svelte';
    import Icon from './Icon.svelte';
    let {
        label,
        summary,
        truncateSummary = false,
        members = [],
        actions,
        disabled = false,
        onremove,
    }: {
        label: string;
        summary: string;
        truncateSummary?: boolean;
        members?: { id: number; name: string }[];
        actions?: Snippet;
        disabled?: boolean;
        onremove?: (id: number) => void;
    } = $props();
</script>

<div class="import-source-header">
    <div class="source-summary">
        <strong title={label}>{label}</strong><small class:truncate={truncateSummary}
            >{#if truncateSummary}<AttributeHelp label={summary} description={summary} />{:else}{summary}{/if}</small
        >
        <div class="source-actions">{@render actions?.()}</div>
    </div>
    {#if members.length}
        <!-- svelte-ignore a11y_no_noninteractive_tabindex (The bounded disk list supports keyboard scrolling.) -->
        <div
            class="source-members"
            class:multiple={members.length > 1}
            role="region"
            aria-label="Source disks"
            tabindex="0"
        >
            {#each members as member (member.id)}
                <div class="source-member">
                    <span class="member-name"><AttributeHelp label={member.name} description={member.name} /></span>
                    {#if onremove}<button
                            class="icon-button"
                            aria-label={`Remove ${member.name}`}
                            title={`Remove ${member.name}`}
                            {disabled}
                            onclick={() => onremove?.(member.id)}><Icon name="close" size={12} /></button
                        >{/if}
                </div>
            {/each}
        </div>
    {/if}
</div>

<style>
    .import-source-header {
        flex: none;
        min-width: 0;
    }
    .source-summary {
        display: flex;
        align-items: center;
        gap: 12px;
        min-height: 30px;
    }
    strong {
        font-size: var(--dialog-section-font-size);
        min-width: 0;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
    }
    small {
        min-width: 0;
        overflow-wrap: anywhere;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .source-actions {
        display: flex;
        flex: none;
        align-items: center;
        gap: 4px;
        margin-left: auto;
    }
    .truncate :global(button) {
        display: block;
        max-width: 100%;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
    }
    .source-members {
        display: grid;
        max-height: 52px;
        overflow-y: auto;
        scrollbar-gutter: stable;
    }
    .source-members.multiple {
        grid-template-columns: repeat(2, minmax(0, 1fr));
        column-gap: 12px;
    }
    .source-member {
        display: flex;
        gap: 8px;
        align-items: center;
        min-width: 0;
        height: 26px;
    }
    .member-name {
        min-width: 0;
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .multiple .member-name {
        flex: 1;
    }
    .member-name :global(button) {
        display: block;
        max-width: 100%;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
    }
    .source-members:focus-visible {
        outline: 1px solid var(--state-focus);
        outline-offset: -1px;
    }
    @container disk-import (max-width: 600px) {
        .source-summary {
            flex-wrap: wrap;
            gap: 4px 8px;
        }
        small {
            order: 3;
            flex-basis: 100%;
        }
        .source-members.multiple {
            grid-template-columns: minmax(0, 1fr);
        }
    }
</style>
