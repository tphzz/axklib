<script lang="ts">
    import { untrack } from 'svelte';
    import type { AppProps } from './appProps';
    import type { WorkspaceBackend } from './features/workspace/contracts';
    import { workspaceBackends } from './features/backends/registry';
    import { provideInspectorPanels } from './lib/inspectorPanels.svelte';
    import { ASeriesPreferences, provideASeriesPreferences } from './lib/aSeriesPreferences.svelte';
    provideInspectorPanels();
    let {
        backend = workspaceBackends[0],
        aSeriesPreferences = new ASeriesPreferences(),
        ...options
    }: AppProps & { backend?: WorkspaceBackend } = $props();
    provideASeriesPreferences(untrack(() => aSeriesPreferences));
    const Application = $derived(backend.application);
</script>

<svelte:head><title>axkdeck</title></svelte:head>
<Application {...options} />
