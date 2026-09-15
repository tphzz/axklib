<script lang="ts">
    import ObjectInspector from '../lib/components/ObjectInspector.svelte';
    import InspectorModeFooter from '../lib/components/InspectorModeFooter.svelte';
    import { inspectorRelationshipFixture } from './inspectorRelationshipFixture';

    const parameters = new URLSearchParams(window.location.search);
    const width = Number(parameters.get('width') ?? 320);
    const scale = Number(parameters.get('scale') ?? 1);
    const selection = inspectorRelationshipFixture(parameters.get('kind') ?? 'sample');
    let navigation = $state('');
</script>

<div class="fixture" style:width={`${width}px`} style:zoom={scale} data-navigation={navigation}>
    <ObjectInspector {selection} onrelationshipnavigate={(id, focus) => (navigation = `${id}:${focus}`)} />
    <InspectorModeFooter mode="files" onclick={() => {}} />
</div>

<style>
    .fixture {
        display: flex;
        flex-direction: column;
        height: 620px;
        margin: 12px;
        background: var(--color-panel);
    }
    .fixture :global(.inspector) {
        flex: 1;
        min-height: 0;
    }
</style>
