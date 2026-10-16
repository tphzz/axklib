<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import CollectionRow from '../lib/components/CollectionRow.svelte';
    import CollectionIdentity from '../lib/components/CollectionIdentity.svelte';

    let props: Omit<ComponentProps<typeof CollectionRow>, 'children'> = $props();
</script>

<CollectionRow {...props}>
    <CollectionIdentity name="12string" metadata="146 B" />
</CollectionRow>
