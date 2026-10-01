import { getContext, setContext } from 'svelte';
import type { ImageTransport } from '../../lib/transport';
import type { AuditionWorkflow } from '../audition/workflow.svelte';

// The editor uses the workspace audio service, so only one audition can own output.
export interface EditorAudioServices {
    transport: ImageTransport;
    audition: Pick<AuditionWorkflow, 'state' | 'autoplay' | 'playPrepared' | 'seekPrepared' | 'stop'>;
}
const key = Symbol('editor-audio');
export const provideEditorAudio = (services: EditorAudioServices) => setContext(key, services);
export const editorAudio = (): EditorAudioServices | undefined => getContext(key);
