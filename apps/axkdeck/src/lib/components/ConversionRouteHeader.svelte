<script lang="ts">
    import AttributeHelp from './AttributeHelp.svelte';
    import SelectControl from './SelectControl.svelte';
    import Icon from './Icon.svelte';
    let {
        source,
        sourceLabel,
        profile,
        targetLabel,
        disabled,
        diskIcon = false,
    }: {
        source?: string;
        sourceLabel: string;
        profile?: string;
        targetLabel?: string;
        disabled: boolean;
        diskIcon?: boolean;
    } = $props();
</script>

<div class="route">
    <span>Source</span>{#if diskIcon}<Icon name="disc" size={14} />{/if}<strong>{sourceLabel}</strong>
    <span class="path"
        >{#if source}<AttributeHelp label={source} description={source} />{/if}</span
    >
    <div class="target-format">
        <strong>Target format</strong>
        <SelectControl
            label="Target format"
            class="format-select"
            value={profile}
            options={profile && targetLabel ? [{ value: profile, label: targetLabel }] : []}
            disabled={disabled || !profile}
            onchange={() => undefined}
        />
    </div>
</div>

<style>
    .route,
    .target-format {
        display: flex;
        align-items: center;
        gap: 8px;
        min-width: 0;
    }
    .route {
        flex: none;
        flex-wrap: wrap;
        min-height: 32px;
        font-size: var(--dialog-body-font-size);
    }
    .route span {
        color: var(--color-text-muted);
    }
    .target-format {
        max-width: 100%;
    }
    .target-format strong {
        white-space: nowrap;
    }
    .route :global(.format-select) {
        flex: 0 1 230px;
        width: 230px;
        max-width: 100%;
    }
    .path {
        flex: 1;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    @container disk-import (max-width:680px) {
        .path {
            flex-basis: 45%;
        }
    }
</style>
