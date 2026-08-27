<script lang="ts">
    import { objectSizeSummary, objectSizeTooltip } from '../objectSizePresentation';
    import type { SamplerObject } from '../transport';
    import Icon from './Icon.svelte';

    interface Props {
        name: string;
        object: SamplerObject;
        metadata?: string;
        indicator?: 'stereo';
    }

    let { name, object, metadata = '', indicator }: Props = $props();
    const tooltip = $derived(objectSizeTooltip(object));
</script>

<span class="object-size-primary">
    <strong title={tooltip}>{name}</strong>
    {#if indicator === 'stereo'}
        <span class="object-size-indicator" role="img" aria-label="Stereo Sample" title="Stereo Sample">
            <Icon name="stereo" size={12} />
        </span>
    {/if}
</span>
<small class="object-size-secondary" title={tooltip}>
    {metadata ? `${metadata} · ` : ''}{objectSizeSummary(object)}
</small>

<style>
    .object-size-primary {
        display: flex;
        min-width: 0;
        align-items: center;
        gap: 3px;
    }

    .object-size-primary strong {
        display: block;
        min-width: 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 10px;
        font-weight: 600;
        line-height: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .object-size-secondary {
        display: block;
        margin-top: 0;
        overflow: hidden;
        color: var(--color-text-muted);
        font-size: 8.5px;
        line-height: 9px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .object-size-indicator {
        display: inline-flex;
        flex: 0 0 auto;
        color: var(--color-accent);
    }
</style>
