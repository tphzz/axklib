<script lang="ts">
    import { onMount } from 'svelte';
    import Workspace from '../features/backends/axklib/Workspace.svelte';
    import { capacityRefreshFixture } from './capacityRefreshFixture.svelte';
    let { fixture = capacityRefreshFixture() } = $props<{ fixture?: ReturnType<typeof capacityRefreshFixture> }>();
    let ready = $state(false);
    onMount(() => {
        void fixture.open().then(() => (ready = true));
        return () => {
            fixture.release();
            void fixture.workflow.dispose();
        };
    });
</script>

<nav aria-label="Refresh fixture controls">
    <button onclick={() => void fixture.refresh()}>Complete floppy import</button>
    <button onclick={() => void fixture.refresh('FITS', 'New import')}>Complete package import</button>
    <button onclick={() => void fixture.refresh('FITS', 'foo', true)}>Complete audio import</button>
    <button onclick={() => void fixture.refresh('DOES_NOT_FIT')}>Exceed capacity</button>
    <button onclick={() => void fixture.refresh('ERROR')}>Fail inspection</button>
    <button onclick={() => fixture.recover()}>Restore inspection</button>
    <button onclick={() => (fixture.hold = !fixture.hold)}
        >{fixture.hold ? 'Resume responses' : 'Delay responses'}</button
    >
    <button onclick={() => fixture.release()}>Release responses</button>
</nav>
{#if ready}<Workspace {...fixture.props} />{/if}
<output data-inspections>{JSON.stringify({ calls: fixture.calls, pending: fixture.pendingCount })}</output>

<style>
    nav {
        display: flex;
        flex-wrap: wrap;
        gap: 4px;
        padding: 4px;
    }
    nav button {
        font-size: 11px;
    }
    output {
        position: fixed;
        bottom: 0;
        left: 0;
        font-size: 9px;
        pointer-events: none;
    }
    :global(.app-shell) {
        height: calc(100vh - 60px);
        min-height: 0;
    }
</style>
