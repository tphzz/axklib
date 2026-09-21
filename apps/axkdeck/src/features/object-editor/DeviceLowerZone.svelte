<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import ObjectEditor from '../../lib/components/ObjectEditor.svelte';
    import DeviceEditorHost from './DeviceEditorHost.svelte';
    let { sessionId, ...props }: ComponentProps<typeof ObjectEditor> & { sessionId: number | null } = $props();
</script>

{#if props.selection?.kind === 'sample' || props.selection?.kind === 'sample-bank'}
    <DeviceEditorHost {sessionId} selection={props.selection} />
{:else}
    <ObjectEditor {...props} />
{/if}
