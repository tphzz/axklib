<script lang="ts">
    import { onMount, onDestroy } from 'svelte';
    import Workspace from '../features/backends/axklib/Workspace.svelte';
    import { provideASeriesPreferences, ASeriesPreferences } from '../lib/aSeriesPreferences.svelte';
    import { imageScopeFixture } from './imageScopeFixture.svelte';
    provideASeriesPreferences(new ASeriesPreferences());
    let { fixture = imageScopeFixture() }: { fixture?: ReturnType<typeof imageScopeFixture> } = $props();
    onMount(() => {
        void fixture.open();
    });
    onDestroy(() => {
        void fixture.workflow.dispose();
    });
</script>

<Workspace {...fixture.props} />
<div class="test-controls">
    <label><input type="checkbox" bind:checked={fixture.blocked} /> Block switching</label>
    <label><input type="checkbox" bind:checked={fixture.decline} /> Decline switch</label>
    <label><input type="checkbox" bind:checked={fixture.fail} /> Fail switch</label>
    <button onclick={() => void fixture.refresh()}>Refresh fixture</button>
    <output aria-label="Fixture closed">{fixture.closed}</output>
</div>

<style>
    .test-controls {
        position: fixed;
        bottom: 0;
        left: 0;
        z-index: 10;
        font-size: 10px;
        background: #181818;
    }
</style>
