<script lang="ts">
    import AttributeHelp from '../../lib/components/AttributeHelp.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    let { label, description, warning = false }: { label: string; description: string; warning?: boolean } = $props();
    let anchor = $state<HTMLElement>();
    function rowHelp(node: HTMLElement) {
        const row = node.closest('button');
        if (row && !row.hasAttribute('aria-describedby')) anchor = row;
        return {
            destroy() {
                anchor = undefined;
            },
        };
    }
</script>

<span class="format-badge" title={anchor ? undefined : description} aria-label={description} use:rowHelp>{label}</span>
{#if warning}<span class="warning" title={description} aria-label="Stored parameter warning"
        ><Icon name="triangle-alert" size={11} /></span
    >{/if}
{#if anchor}<AttributeHelp {anchor} {label} {description} focusVisibleOnly />{/if}

<style>
    .format-badge {
        display: inline-flex;
        flex: 0 0 auto;
        align-items: center;
        justify-content: center;
        height: 13px;
        padding: 0 3px;
        border: 1px solid currentColor;
        border-radius: 3px;
        color: var(--color-text-muted);
        font-size: 8px;
        line-height: 11px;
        white-space: nowrap;
    }
    .warning {
        color: var(--color-warning, #e6a34c);
        display: inline-flex;
        flex: none;
        align-items: center;
    }
</style>
