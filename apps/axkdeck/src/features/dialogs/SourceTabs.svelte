<script lang="ts">
    import { tick } from 'svelte';
    import { sourceTabs, type SourceTab } from './sourceTabs';
    let {
        id,
        label,
        value,
        available,
        reason = '',
        disabled = false,
        onchange,
    }: {
        id: string;
        label: string;
        value: SourceTab;
        available: boolean;
        reason?: string;
        disabled?: boolean;
        onchange: (value: SourceTab) => void | Promise<void>;
    } = $props();
    async function navigate(event: KeyboardEvent) {
        if (disabled || !['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
        event.preventDefault();
        const element = event.currentTarget as HTMLElement;
        const tabs: readonly SourceTab[] = available ? sourceTabs : ['Image'];
        const at = tabs.indexOf(value);
        const next =
            event.key === 'Home'
                ? 0
                : event.key === 'End'
                  ? tabs.length - 1
                  : (at + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
        await onchange(tabs[next]!);
        await tick();
        element.querySelector<HTMLButtonElement>(`[data-tab="${tabs[next]}"]`)?.focus();
    }
</script>

<div
    class="source-tabs dialog-segmented-control"
    role="tablist"
    aria-label={label}
    tabindex="-1"
    onkeydown={(event) => void navigate(event)}
>
    {#each sourceTabs as tab}
        <button
            type="button"
            role="tab"
            id={`${id}-tab-${tab.replaceAll(' ', '-')}`}
            data-tab={tab}
            aria-selected={value === tab}
            aria-controls={`${id}-panel-${tab.replaceAll(' ', '-')}`}
            tabindex={value === tab ? 0 : -1}
            disabled={disabled || (tab !== 'Image' && !available)}
            title={tab !== 'Image' && !available ? reason : undefined}
            aria-describedby={tab !== 'Image' && !available ? `${id}-device-unavailable` : undefined}
            onclick={() => void onchange(tab)}>{tab}</button
        >
    {/each}
</div>
{#if !available && reason}<p id={`${id}-device-unavailable`} class="source-reason">{reason}</p>{/if}

<style>
    .source-tabs {
        margin: 10px 12px;
        flex: 0 0 auto;
        width: max-content;
        max-width: calc(100% - 24px);
    }
    .source-reason {
        margin: 0 12px 10px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
</style>
