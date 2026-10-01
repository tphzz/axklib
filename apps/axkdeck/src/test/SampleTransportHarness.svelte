<script lang="ts">
    import { untrack } from 'svelte';
    import SampleTransport from '../features/devices/a-series/sample/SampleTransport.svelte';
    import { provideEditorAudio, type EditorAudioServices } from '../features/object-editor/audioContext';
    import type { ObjectEditorDocument } from '../features/object-editor/workflow.svelte';
    import type { AuditionState } from '../lib/audio/auditionController';

    let {
        document,
        services,
        onready,
    }: {
        document: ObjectEditorDocument;
        services: EditorAudioServices;
        onready: (update: (state: AuditionState) => void) => void;
    } = $props();
    let audioState = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    let autoplay = $state(false);
    untrack(() => {
        provideEditorAudio({
            transport: services.transport,
            audition: {
                ...services.audition,
                get state() {
                    return audioState;
                },
                get autoplay() {
                    return autoplay;
                },
                set autoplay(value) {
                    autoplay = value;
                },
            },
        });
        onready((value) => (audioState = value));
    });
</script>

<SampleTransport {document} rate={1000} disabled={false} />
