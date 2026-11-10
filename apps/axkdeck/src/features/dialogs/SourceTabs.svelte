<script lang="ts">
    import TabStrip from '../../lib/components/TabStrip.svelte';
    import TabBar from '../../lib/components/TabBar.svelte';
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
    const tabs = $derived(
        sourceTabs.map((tab) => ({
            id: tab,
            label: tab,
            buttonId: `${id}-tab-${tab.replaceAll(' ', '-')}`,
            panelId: `${id}-panel-${tab.replaceAll(' ', '-')}`,
            disabled: tab !== 'Image' && !available,
            reason: tab !== 'Image' && !available ? reason : undefined,
            describedBy: tab !== 'Image' && !available && reason ? `${id}-device-unavailable` : undefined,
        })),
    );
</script>

<div class="source-tabs">
    <TabBar context="dialog">
        <TabStrip {tabs} active={value} {label} {disabled} onselect={onchange} />
    </TabBar>
</div>
{#if !available && reason}<p id={`${id}-device-unavailable`} class="source-reason">{reason}</p>{/if}

<style>
    .source-tabs {
        margin: 10px 0;
        flex: 0 0 auto;
        min-width: 0;
    }
    .source-reason {
        margin: 0 12px 10px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
</style>
