<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import Presentation from '../features/devices/tx16w/Presentation.svelte';
    let props: Omit<ComponentProps<typeof Presentation>, 'children'> = $props();
</script>

<Presentation {...props}>
    {#snippet children(presentation, state)}
        {@render presentation.navigation()}
        {#if presentation.tabs}{@render presentation.tabs()}{/if}
        {@render presentation.content()}
        {#if presentation.inspector}{@render presentation.inspector()}{/if}
        <button onclick={() => void state.showObject('wave-20')}>Reveal wave</button>
    {/snippet}
</Presentation>
