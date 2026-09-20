<script lang="ts">
    import { untrack } from 'svelte';
    import DeviceEditorHost from '../features/object-editor/DeviceEditorHost.svelte';
    import { provideObjectEditors } from '../features/object-editor/context';
    import type { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import type { InspectorSelection } from '../lib/types';

    let {
        workflow,
        sessionId = 1,
        sample = 'A',
        visible = true,
    }: {
        workflow: ObjectEditorWorkflow;
        sessionId?: number;
        sample?: string;
        visible?: boolean;
    } = $props();
    provideObjectEditors(untrack(() => workflow));
    const selection = $derived({
        kind: 'sample',
        item: { objectId: sample },
        preview: { preview: { lanes: [] } },
    } as unknown as InspectorSelection);
</script>

{#if visible}<DeviceEditorHost {sessionId} {selection} />{/if}
