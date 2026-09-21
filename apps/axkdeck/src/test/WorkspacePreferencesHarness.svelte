<script lang="ts">
    import WorkspaceShell from '../features/workspace/WorkspaceShell.svelte';
    import { ASeriesPreferences, provideASeriesPreferences } from '../lib/aSeriesPreferences.svelte';

    const preferences = new ASeriesPreferences();
    provideASeriesPreferences(preferences);
</script>

{#snippet empty()}{/snippet}
{#snippet content()}
    <output aria-label="Saved generation">{preferences.generation}</output>
{/snippet}

<WorkspaceShell
    mode="device"
    presentation={{ navigation: empty, content }}
    imageActions={empty}
    isDesktop={true}
    onmodechange={() => undefined}
/>
