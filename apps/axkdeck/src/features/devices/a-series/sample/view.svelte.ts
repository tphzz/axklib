import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import type { EditorAudioSource } from '../../../object-editor/audioSource';
import type { EditorAudioServices } from '../../../object-editor/audioContext';
import { loadEditorAudio } from '../../../object-editor/audioSource';
import { userFacingMessage } from '../../../../lib/userFacingMessage';

export class SampleView {
    units = $state(0);
    zoom = $state(1);
    pan = $state(0);
    snap = $state(false);
    beats = $state(2);
    monitorMs = $state(-30);
    note = $state(60);
    volume = $state(65);
    cursor = $state(0);
    parameterGroups = $state<Record<string, string>>({});
    lfoTraces = $state(['pitch', 'amp']);
    source = $state.raw<EditorAudioSource>();
    loading = $state(false);
    error = $state('');
    gain: GainNode | undefined;
    private request: AbortController | undefined;
    private pending: Promise<EditorAudioSource | undefined> | undefined;

    async load(
        document: ObjectEditorDocument,
        audio: EditorAudioServices | undefined,
    ): Promise<EditorAudioSource | undefined> {
        if (this.source) return this.source;
        if (this.pending) return this.pending;
        if (!audio) {
            this.error = 'Source audio is unavailable';
            return;
        }
        const request = new AbortController();
        this.request = request;
        this.loading = true;
        this.error = '';
        this.pending = (async () => {
            try {
                const source = await loadEditorAudio(
                    audio.transport,
                    document.sessionId,
                    document.detail!.object.id,
                    new OfflineAudioContext(1, 1, 44100),
                    request.signal,
                );
                if (!request.signal.aborted) this.source = source;
                return this.source;
            } catch (error) {
                if (!request.signal.aborted) this.error = userFacingMessage(error);
                return undefined;
            } finally {
                if (this.request === request) {
                    this.loading = false;
                    this.pending = undefined;
                }
            }
        })();
        return this.pending;
    }
    release() {
        this.request?.abort();
        this.request = undefined;
        this.pending = undefined;
        this.source = undefined;
        this.loading = false;
        this.gain?.disconnect();
        this.gain = undefined;
    }
}
const views = new WeakMap<ObjectEditorDocument, SampleView>();
export function sampleView(document: ObjectEditorDocument): SampleView {
    let view = views.get(document);
    if (!view) {
        view = new SampleView();
        view.note = Number(document.draft.values.root_key ?? 60);
        views.set(document, view);
    }
    return view;
}
