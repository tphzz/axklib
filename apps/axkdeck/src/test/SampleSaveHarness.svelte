<script lang="ts">
    import EditorBoundary from '../features/object-editor/EditorBoundary.svelte';
    import DeviceEditorHost from '../features/object-editor/DeviceEditorHost.svelte';
    import type { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
    import type { AuditionWorkflow } from '../features/audition/workflow.svelte';
    import type { ImageTransport } from '../lib/transport';
    import type { InspectorSelection } from '../lib/types';

    let {
        transport,
        imageSession,
        audition,
    }: {
        transport: ImageTransport;
        imageSession: ImageSessionWorkflow;
        audition: AuditionWorkflow;
    } = $props();
    const selection = {
        kind: 'sample',
        item: { objectId: 'sample' },
        preview: { preview: { lanes: [] } },
    } as unknown as InspectorSelection;
</script>

<EditorBoundary {transport} {imageSession} {audition}>
    <DeviceEditorHost sessionId={1} {selection} />
</EditorBoundary>
