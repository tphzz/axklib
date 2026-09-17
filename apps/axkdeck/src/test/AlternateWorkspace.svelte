<script lang="ts">
    import type { AppProps } from '../appProps';
    let { ...options }: AppProps = $props();
    import WorkspaceShell from '../features/workspace/WorkspaceShell.svelte';
    import type { WorkspaceMode } from '../features/workspace/contracts';
    let mode = $state<WorkspaceMode>('device');
</script>

{#snippet imageActions()}<button type="button">Open image</button>{/snippet}
{#snippet navigation()}<p>{mode} navigation</p>{/snippet}
{#snippet content()}<section>{mode} content</section>{/snippet}
{#snippet lower()}<section>Alternate device tools</section>{/snippet}
{#snippet inspector()}<aside>Alternate {mode} inspector</aside>{/snippet}
<WorkspaceShell
    {mode}
    {imageActions}
    onmodechange={(value) => (mode = value)}
    presentation={{ navigation, content, inspector, lower: mode === 'device' ? lower : undefined }}
/>
