<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import Tx16wPresentationHarness from '../../../test/Tx16wPresentationHarness.svelte';
    let props: ComponentProps<typeof Tx16wPresentationHarness> = $props();
    let visible = $state(true);
</script>

<button onclick={() => (visible = !visible)}>Switch presentation visibility</button>
<Tx16wPresentationHarness {...props} {visible} />
