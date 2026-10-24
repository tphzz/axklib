import type { ImageTransport, PreviewEnvelope } from '../transport';
import { userFacingMessage } from '../userFacingMessage';
import { AuditionController, type AuditionState } from './auditionController';
import { WaveformPreviewQueue } from './waveformPreviewQueue';
import type { WaveformTimeline } from '../waveformTimeline';

export function previewTimeline(preview?: PreviewEnvelope): WaveformTimeline {
    const lane = preview?.lanes[0];
    return {
        sampleRate: lane?.sampleRate ?? 0,
        storedFrameCount: lane?.storedFrameCount ?? 0,
        playbackStartFrame: lane?.playbackStartFrame ?? 0,
        playbackLengthFrames: lane?.playbackLengthFrames ?? 0,
        loopStartFrame: lane?.loopStartFrame ?? 0,
        loopLengthFrames: lane?.loopLengthFrames ?? 0,
        displayDurationSeconds: lane ? lane.storedFrameCount / lane.sampleRate : 0,
    };
}

export class DocumentAudio {
    previews = $state<Record<string, PreviewEnvelope>>({});
    errors = $state<Record<string, string>>({});
    state = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    private pending = new Set<string>();
    private queue: WaveformPreviewQueue;
    private controller: AuditionController;

    constructor(
        transport: ImageTransport,
        private sessionId: () => number,
    ) {
        this.queue = new WaveformPreviewQueue(transport);
        this.controller = new AuditionController(transport, (state) => {
            this.state = state;
            this.queue.playbackPending(state.status === 'preparing');
        });
    }

    request(objectId: string, bins = 1024): void {
        const previous = this.previews[objectId]?.lanes[0];
        const key = `${objectId}/${bins}`;
        if (
            this.pending.has(key) ||
            this.errors[objectId] ||
            (previous && previous.bins.length >= Math.min(bins, previous.storedFrameCount))
        )
            return;
        this.pending.add(key);
        this.queue.request(
            this.sessionId(),
            objectId,
            bins,
            (preview) => {
                if ((this.previews[objectId]?.lanes[0]?.bins.length ?? 0) <= preview.lanes[0]!.bins.length)
                    this.previews[objectId] = preview;
                this.pending.delete(key);
            },
            (error) => {
                this.errors[objectId] = userFacingMessage(error);
                this.pending.delete(key);
            },
        );
    }

    play(objectId: string): void {
        void this.controller.play(this.sessionId(), objectId);
    }
    prefetch(objectId: string): void {
        void this.controller.prefetch(this.sessionId(), objectId);
    }
    seek(objectId: string, ratio: number): void {
        const lane = this.previews[objectId]?.lanes[0];
        if (lane && this.state.objectId === objectId)
            this.controller.seek(
                Math.max(
                    0,
                    Math.min(
                        lane.playbackLengthFrames - 1,
                        Math.floor(lane.storedFrameCount * ratio) - lane.playbackStartFrame,
                    ),
                ),
            );
    }
    stop(): void {
        void this.controller.stop();
    }
    reset(): void {
        this.queue.reset();
        this.pending.clear();
        this.previews = {};
        this.errors = {};
        void this.controller.invalidateSession(this.sessionId());
    }
    dispose(): void {
        this.queue.reset();
        void this.controller.dispose();
    }
}
