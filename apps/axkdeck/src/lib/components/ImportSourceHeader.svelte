<script lang="ts">
    import type { Snippet } from 'svelte';
    import { tick } from 'svelte';
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
        members?: { id: number; name: string; autoAdded?: boolean }[];
        actions?: Snippet;
        disabled?: boolean;
        onremove?: (id: number) => void | Promise<void>;
    } = $props();
    let root: HTMLDivElement;
    async function remove(id: number, button: HTMLButtonElement) {
        const restoreFocus = document.activeElement === button;
        const dialog = root.closest('[role="dialog"]') ?? root;
        const index = members.findIndex((member) => member.id === id);
        const next = members[index + 1]?.id;
        const previous = members[index - 1]?.id;
        await onremove?.(id);
        await tick();
        if (
            !restoreFocus ||
            !dialog.isConnected ||
            (document.activeElement !== button && document.activeElement !== document.body)
        )
            return;
        const target = [id, next, previous].find(
            (key) => key !== undefined && dialog.querySelector(`[data-remove-member="${key}"]:not([disabled])`),
        );
        const control =
            target !== undefined
                ? dialog.querySelector<HTMLElement>(`[data-remove-member="${target}"]`)
                : dialog.querySelector<HTMLElement>(
                      '.source-actions button:not([disabled]), .source-actions input:not([disabled]), .import-source-choice button:not([disabled])',
                  );
        control?.focus();
    }
</script>

<div class="import-source-header" bind:this={root}>
    <div class="source-summary">
        <strong title={label}>{label}</strong><small class:truncate={truncateSummary}
            >{#if truncateSummary}<AttributeHelp label={summary} description={summary} />{:else}{summary}{/if}</small
        >
        <div class="source-actions">{@render actions?.()}</div>
    </div>
    {#if members.length}
        <!-- svelte-ignore a11y_no_noninteractive_tabindex (The bounded disk list supports keyboard scrolling.) -->
        <div class="source-members" role="region" aria-label="Source disks" tabindex="0">
            {#each members as member (member.id)}
                <div class="source-member">
                    <span class="member-name"><AttributeHelp label={member.name} description={member.name} /></span>
                    {#if member.autoAdded}<span class="member-origin">Auto-added</span>{/if}
                    {#if onremove}<button
                            class="icon-button"
                            aria-label={`Remove ${member.name}`}
                            title={`Remove ${member.name}`}
                            {disabled}
                            data-remove-member={member.id}
                            onclick={(event) => remove(member.id, event.currentTarget)}
                            ><Icon name="close" size={14} /></button
                        >{/if}
                </div>
            {/each}
        </div>
    {/if}
</div>

<style>
    .import-source-header {
        display: flex;
        flex-direction: column;
        gap: 4px;
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
        display: flex;
        flex-wrap: wrap;
        align-content: flex-start;
        gap: 4px;
        max-height: 56px;
        overflow-y: auto;
        padding-right: var(--overlay-scrollbar-clearance);
    }
    .source-member {
        display: flex;
        flex: 0 1 auto;
        gap: 4px;
        align-items: center;
        min-width: 0;
        max-width: 100%;
        height: 26px;
        box-sizing: border-box;
        padding-left: 6px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        background: var(--color-bg-deep);
    }
    .member-name {
        min-width: 0;
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .member-origin {
        flex: none;
        white-space: nowrap;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .source-member .icon-button {
        flex: none;
        width: 26px;
        height: 24px;
        min-height: 24px;
    }
    .source-member :global(button:focus-visible) {
        outline-offset: -2px;
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
    }
</style>
