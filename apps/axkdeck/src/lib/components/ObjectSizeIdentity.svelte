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
<small title={tooltip}>{metadata ? `${metadata} · ` : ''}{objectSizeSummary(object)}</small>
