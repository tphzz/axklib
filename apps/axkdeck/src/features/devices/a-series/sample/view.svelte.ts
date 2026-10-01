import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import type { EditorAudioSource } from '../../../object-editor/audioSource';
import type { EditorAudioServices } from '../../../object-editor/audioContext';
import { loadEditorAudio } from '../../../object-editor/audioSource';
import { userFacingMessage } from '../../../../lib/userFacingMessage';

class SamplePreferences {
    units = $state(0);
    endType = $state(0);
    beats = $state(4);
    normalizeTempo = $state(true);
    monitorMs = $state(-30);
}
const preferences = new WeakMap<object, SamplePreferences>();

export class SampleView {
    constructor(readonly preferences = new SamplePreferences()) {}
    get units() {
        return this.preferences.units;
    }
    set units(value: number) {
        this.preferences.units = value;
    }
    get monitorMs() {
        return this.preferences.monitorMs;
    }
    set monitorMs(value: number) {
        this.preferences.monitorMs = value;
    }
    zoom = $state(1);
    pan = $state(0);
    snap = $state(false);
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
        const settings = preferences.get(document.preferencesScope) ?? new SamplePreferences();
        preferences.set(document.preferencesScope, settings);
        view = new SampleView(settings);
        view.note = Number(document.draft.values.root_key ?? 60);
        views.set(document, view);
    }
    return view;
}
