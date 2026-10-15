<script lang="ts">
    import { untrack, type ComponentProps } from 'svelte';
    import { InspectorPanels, provideInspectorPanels } from '../../../lib/inspectorPanels.svelte';
    import ObjectDetail from './ObjectDetail.svelte';

    let {
        panels,
        visible = true,
        ...props
    }: ComponentProps<typeof ObjectDetail> & { panels: InspectorPanels; visible?: boolean } = $props();
    provideInspectorPanels(untrack(() => panels));
</script>

{#if visible}
    {#key props.detail?.documentId}<ObjectDetail {...props} />{/key}
{/if}
