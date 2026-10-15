import type { AudioIdentity, ImageTransport, PreviewEnvelope } from '../transport';
import { sameAudioIdentity } from './audioIdentity';

interface Request {
    sessionId: number;
    identity: AudioIdentity;
    objectId: string;
    bins: number;
    generation: number;
    loaded: (preview: PreviewEnvelope) => void;
    failed: (error: unknown) => void;
}

// One bounded queue is shared by all device presentations; reset aborts old document reads.
export class WaveformPreviewQueue {
    private queued: Request[] = [];
    private active = new Set<AbortController>();
    private generation = 0;

    constructor(private transport: ImageTransport) {}

    request(
        sessionId: number,
        objectId: string,
        bins: number,
        loaded: Request['loaded'],
        failed: Request['failed'],
    ): void {
        this.queued.push({
            sessionId,
            identity: this.transport.audioIdentity(sessionId),
            objectId,
            bins,
            generation: this.generation,
            loaded,
            failed,
        });
        this.drain();
    }

    reset(): void {
        this.generation++;
        this.queued = [];
        for (const controller of this.active) controller.abort();
    }

    private current(request: Request): boolean {
        try {
            return (
                request.generation === this.generation &&
                sameAudioIdentity(request.identity, this.transport.audioIdentity(request.sessionId))
            );
        } catch {
            return false;
        }
    }

    private drain(): void {
        while (this.active.size < 2 && this.queued.length) {
            const request = this.queued.shift()!;
            if (!this.current(request)) continue;
            const controller = new AbortController();
            this.active.add(controller);
            void this.transport
                .preview(request.sessionId, request.objectId, request.bins, controller.signal)
                .then((preview) => {
                    if (!this.current(request) || controller.signal.aborted) return;
                    if (!sameAudioIdentity(request.identity, preview) || preview.objectId !== request.objectId)
                        throw new Error('Waveform belongs to a different native document');
                    request.loaded(preview);
                })
                .catch((error: unknown) => {
                    if (this.current(request) && !controller.signal.aborted) request.failed(error);
                })
                .finally(() => {
                    this.active.delete(controller);
                    this.drain();
                });
        }
    }
}
