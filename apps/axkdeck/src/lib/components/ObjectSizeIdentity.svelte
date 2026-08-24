<script lang="ts">
    import { objectSizeSummary, objectSizeTooltip } from '../objectSizePresentation';
    import type { SamplerObject } from '../transport';

    interface Props {
        name: string;
        object: SamplerObject;
        metadata?: string;
    }

    let { name, object, metadata = '' }: Props = $props();
    const tooltip = $derived(objectSizeTooltip(object));
</script>

<strong title={tooltip}>{name}</strong>
<small title={tooltip}>{metadata ? `${metadata} · ` : ''}{objectSizeSummary(object)}</small>
