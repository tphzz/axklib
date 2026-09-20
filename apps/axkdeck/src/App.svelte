<script lang="ts">
    import type { AppProps } from './appProps';
    import type { WorkspaceBackend } from './features/workspace/contracts';
    import { workspaceBackends } from './features/backends/registry';
    import { provideInspectorPanels } from './lib/inspectorPanels.svelte';
    provideInspectorPanels();
    let { backend = workspaceBackends[0], ...options }: AppProps & { backend?: WorkspaceBackend } = $props();
    const Application = $derived(backend.application);
</script>

<svelte:head><title>axkdeck</title></svelte:head>
<Application {...options} />
