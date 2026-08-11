import { planDirectPlayback } from './directPlaybackSchedule';
import type { PlaybackTimeline } from './playbackTimeline';
import { decodeRiffWavePcm } from './riffWavePcm';

export type AudioImportAuditionStatus = 'idle' | 'preparing' | 'playing' | 'failed';

export interface AudioImportAuditionState {
    rowId: number | null;
    status: AudioImportAuditionStatus;
    error: string;
}

export interface AudioImportAuditionRequest {
    rowId: number;
    loadBlob: () => Promise<Blob>;
    timeline: PlaybackTimeline;
}

interface ActivePlayback {
    rowId: number;
    source: AudioBufferSourceNode;
    gain: GainNode;
}

const startLeadSeconds = 0.01;
const attackSeconds = 0.005;

export class AudioImportAuditionController {
    private context?: AudioContext;
    private active?: ActivePlayback;
    private cached?: { rowId: number; buffer: AudioBuffer };
    private generation = 0;
    private disposed = false;
    private state: AudioImportAuditionState = { rowId: null, status: 'idle', error: '' };

    constructor(private readonly update: (state: AudioImportAuditionState) => void) {}

    async play(request: AudioImportAuditionRequest): Promise<void> {
        if (this.disposed) return;
        this.stopActive();
        const generation = ++this.generation;
        this.setState({ rowId: request.rowId, status: 'preparing', error: '' });
        try {
            const context = this.ensureContext();
            await context.resume();
            const buffer = await this.loadBuffer(context, request);
            if (this.disposed || generation !== this.generation) return;
            this.start(context, request, buffer);
        } catch (error) {
            if (this.disposed || generation !== this.generation) return;
            this.setState({
                rowId: request.rowId,
                status: 'failed',
                error: error instanceof Error ? error.message : String(error),
            });
        }
    }

    stop(rowId?: number): void {
        if (rowId !== undefined && this.state.rowId !== rowId) return;
        ++this.generation;
        this.stopActive();
        this.setState({ rowId: null, status: 'idle', error: '' });
    }

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        ++this.generation;
        this.stopActive();
        this.cached = undefined;
        const context = this.context;
        this.context = undefined;
        if (context && context.state !== 'closed') await context.close().catch(() => undefined);
    }

    private setState(state: AudioImportAuditionState): void {
        this.state = state;
        this.update(state);
    }

    private ensureContext(): AudioContext {
        if (this.context && this.context.state !== 'closed') return this.context;
        this.context = new AudioContext();
        return this.context;
    }

    private async loadBuffer(context: AudioContext, request: AudioImportAuditionRequest): Promise<AudioBuffer> {
        if (this.cached?.rowId === request.rowId) return this.cached.buffer;
        const blob = await request.loadBlob();
        const content = await blob.arrayBuffer();
        const buffer = decodeRiffWavePcm(context, content) ?? (await context.decodeAudioData(content));
        this.cached = { rowId: request.rowId, buffer };
        return buffer;
    }

    private start(context: AudioContext, request: AudioImportAuditionRequest, buffer: AudioBuffer): void {
        const schedule = planDirectPlayback(request.timeline, buffer.duration, 'interactive');
        const source = context.createBufferSource();
        const gain = context.createGain();
        source.buffer = buffer;
        source.loop = schedule.loop;
        source.loopStart = schedule.loopStartSeconds;
        source.loopEnd = schedule.loopEndSeconds;
        source.connect(gain);
        gain.connect(context.destination);

        const startTime = context.currentTime + startLeadSeconds;
        gain.gain.value = 0;
        gain.gain.setValueAtTime(0, context.currentTime);
        gain.gain.setValueAtTime(0, startTime);
        gain.gain.linearRampToValueAtTime(1, startTime + attackSeconds);
        const stopTime = schedule.stopAfterSeconds === null ? null : startTime + schedule.stopAfterSeconds;
        if (stopTime !== null) {
            gain.gain.setValueAtTime(1, stopTime - attackSeconds);
            gain.gain.linearRampToValueAtTime(0, stopTime);
        }
        const active = { rowId: request.rowId, source, gain };
        this.active = active;
        source.onended = () => this.handleEnded(active);
        source.start(startTime);
        if (stopTime !== null) source.stop(stopTime);
        this.setState({ rowId: request.rowId, status: 'playing', error: '' });
    }

    private handleEnded(active: ActivePlayback): void {
        if (this.active !== active) return;
        this.disconnect(active);
        this.active = undefined;
        this.setState({ rowId: null, status: 'idle', error: '' });
    }

    private stopActive(): void {
        const active = this.active;
        if (!active) return;
        this.active = undefined;
        active.source.onended = null;
        try {
            active.source.stop();
        } catch {
            // A source that ended between events is already stopped.
        }
        this.disconnect(active);
    }

    private disconnect(active: ActivePlayback): void {
        active.source.disconnect();
        active.gain.disconnect();
    }
}
