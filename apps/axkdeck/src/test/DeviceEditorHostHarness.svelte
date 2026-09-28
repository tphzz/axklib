<script lang="ts">
    import { untrack } from 'svelte';
    import DeviceEditorHost from '../features/object-editor/DeviceEditorHost.svelte';
    import { provideObjectEditors } from '../features/object-editor/context';
    import { provideEditorAudio, type EditorAudioServices } from '../features/object-editor/audioContext';
    import type { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import type { InspectorSelection, SampleWaveformPreview } from '../lib/types';

    let {
        workflow,
        sessionId = 1,
        sample = 'A',
        visible = true,
        preview = { preview: { lanes: [] } } as unknown as SampleWaveformPreview,
        audio,
    }: {
        workflow: ObjectEditorWorkflow;
        sessionId?: number;
        sample?: string;
        visible?: boolean;
        preview?: SampleWaveformPreview;
        audio?: EditorAudioServices;
    } = $props();
    provideObjectEditors(untrack(() => workflow));
    untrack(() => {
        if (audio) provideEditorAudio(audio);
    });
    const selection = $derived({
        kind: 'sample',
        item: { objectId: sample },
        preview,
    } as unknown as InspectorSelection);
</script>

{#if visible}<DeviceEditorHost {sessionId} {selection} />{/if}
