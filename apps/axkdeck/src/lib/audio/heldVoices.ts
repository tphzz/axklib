import { userFacingMessage } from '../userFacingMessage';
import type { AuditionState } from './auditionTypes';

export interface PreparedVoice {
    buffer: AudioBuffer;
    start: number;
    length: number;
    speed: number;
    gain: number;
    pan: number;
    loop: boolean;
    loopStart: number;
    loopEnd: number;
    stopAfter?: number | null;
}
export type PrepareVoices = (context: AudioContext, signal: AbortSignal) => Promise<PreparedVoice[]>;
interface VoiceNodes {
    source: AudioBufferSourceNode;
    gain: GainNode;
    pan: StereoPannerNode;
}

export class HeldVoices {
    private request: { token: string; sessionId: number; abort: AbortController; nodes: VoiceNodes[] } | null = null;
    get sessionId(): number | undefined {
        return this.request?.sessionId;
    }
    constructor(
        private readonly context: () => AudioContext,
        private readonly claim: () => void,
        private readonly update: (state: AuditionState) => void,
    ) {}
    async play(sessionId: number, objectId: string, token: string, prepare: PrepareVoices): Promise<void> {
        this.claim();
        const request = { token, sessionId, abort: new AbortController(), nodes: [] as VoiceNodes[] };
        this.request = request;
        this.update({ objectId, status: 'preparing', playheadFrame: 0, mapping: true });
        try {
            const context = this.context();
            // Resume before starting network work; the owning webview controls output.
            const resumed = context.resume();
            const [, voices] = await Promise.all([resumed, prepare(context, request.abort.signal)]);
            if (this.request !== request || request.abort.signal.aborted) return;
            if (!voices.length) {
                this.stop(token);
                return;
            }
            if (context.state !== 'running')
                throw new Error('Audio output could not start. Activate audio in the main window and try again.');
            if (
                voices.some(
                    (voice) =>
                        ![
                            voice.start,
                            voice.length,
                            voice.speed,
                            voice.gain,
                            voice.pan,
                            voice.loopStart,
                            voice.loopEnd,
                        ].every(Number.isFinite) ||
                        voice.start < 0 ||
                        voice.length <= 0 ||
                        voice.start + voice.length > voice.buffer.duration ||
                        voice.speed <= 0 ||
                        voice.gain < 0 ||
                        Math.abs(voice.pan) > 1 ||
                        (voice.loop &&
                            (voice.loopStart < 0 ||
                                voice.loopEnd <= voice.loopStart ||
                                voice.loopEnd > voice.buffer.duration)) ||
                        (voice.stopAfter != null && (!Number.isFinite(voice.stopAfter) || voice.stopAfter <= 0)),
                )
            )
                throw new Error('A matching voice has invalid preview settings; no partial group was played.');
            for (const voice of voices) {
                const source = context.createBufferSource(),
                    gain = context.createGain(),
                    pan = context.createStereoPanner();
                request.nodes.push({ source, gain, pan });
                source.buffer = voice.buffer;
                source.playbackRate.value = voice.speed;
                source.loop = voice.loop;
                source.loopStart = voice.loopStart;
                source.loopEnd = voice.loopEnd;
                pan.pan.value = voice.pan;
                source.connect(gain).connect(pan).connect(context.destination);
                source.onended = () => {
                    source.disconnect();
                    gain.disconnect();
                    pan.disconnect();
                    request.nodes = request.nodes.filter((node) => node.source !== source);
                    if (this.request === request && !request.nodes.length) this.stop(token);
                };
            }
            // Every voice is validated and allocated before any is scheduled.
            const when = context.currentTime + 0.01;
            request.nodes.forEach((node, index) => {
                const voice = voices[index]!;
                node.gain.gain.setValueAtTime(0, when);
                node.gain.gain.linearRampToValueAtTime(voice.gain, when + 0.005);
                if (voice.loop) node.source.start(when, voice.start);
                else node.source.start(when, voice.start, voice.length);
                if (voice.stopAfter !== undefined && voice.stopAfter !== null) node.source.stop(when + voice.stopAfter);
            });
            this.update({ objectId, status: 'playing', playheadFrame: 0, mapping: true });
        } catch (error) {
            if (this.request !== request || request.abort.signal.aborted) return;
            this.stop(token);
            this.update({
                objectId,
                status: 'failed',
                playheadFrame: 0,
                mapping: true,
                error: userFacingMessage(error),
            });
        }
    }
    stop(token?: string): void {
        const request = this.request;
        if (!request || (token !== undefined && token !== request.token)) return;
        this.request = null;
        request.abort.abort();
        for (const node of request.nodes) {
            const now = node.source.context.currentTime;
            node.gain.gain.cancelScheduledValues(now);
            node.gain.gain.setValueAtTime(node.gain.gain.value, now);
            node.gain.gain.linearRampToValueAtTime(0, now + 0.005);
            node.source.onended = () => {
                node.source.disconnect();
                node.gain.disconnect();
                node.pan.disconnect();
            };
            try {
                node.source.stop(now + 0.005);
            } catch {
                node.source.disconnect();
                node.gain.disconnect();
                node.pan.disconnect();
            }
        }
        this.update({ objectId: null, status: 'idle', playheadFrame: 0 });
    }
}
