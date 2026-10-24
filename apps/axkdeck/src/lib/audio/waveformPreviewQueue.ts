import type { AudioIdentity, ImageTransport, PreviewEnvelope } from '../transport';
import { sameAudioIdentity } from './audioIdentity';
import { readContext, readTrace, type ReadTrace } from '../readDiagnostics';
import { AxklibApiError } from '../httpApiClient';
import type { PreviewReadOptions } from '../audioReadOptions';

interface Request {
    sessionId: number;
    identity: AudioIdentity;
    objectId: string;
    bins: number;
    generation: number;
    loaded: (preview: PreviewEnvelope) => void;
    failed: (error: unknown) => void;
    intent: PreviewReadOptions['readIntent'];
    cancelled?: () => void;
    trace?: ReadTrace;
}

// One bounded queue is shared by all device presentations; reset aborts old document reads.
export class WaveformPreviewQueue {
    private queued: Request[] = [];
    private active = new Map<AbortController, Request>();
    private generation = 0;
    private playing = false;

    constructor(
        private transport: ImageTransport,
        private intent: PreviewReadOptions['readIntent'] = 'SELECTED',
        private concurrency = 2,
    ) {}

    request(
        sessionId: number,
        objectId: string,
        bins: number,
        loaded: Request['loaded'],
        failed: Request['failed'],
        options: { intent?: PreviewReadOptions['readIntent']; cancelled?: () => void } = {},
    ): void {
        this.queued.push({
            sessionId,
            identity: this.transport.audioIdentity(sessionId),
            objectId,
            bins,
            generation: this.generation,
            loaded,
            failed,
            intent: options.intent ?? this.intent,
            cancelled: options.cancelled,
            trace: readTrace('preview-queue', { sessionId, objectId, bins }, readContext(sessionId, objectId)),
        });
        this.queued.sort((a, b) => Number(a.intent === 'REFINEMENT') - Number(b.intent === 'REFINEMENT'));
        this.drain();
    }

    focus(sessionId: number, objectId: string): void {
        const obsolete = (request: Request) =>
            request.intent === 'SELECTED'
                ? request.sessionId !== sessionId || request.objectId !== objectId
                : request.sessionId === sessionId && request.objectId === objectId;
        this.queued = this.queued.filter((request) => {
            if (!obsolete(request)) return true;
            request.trace?.finish('cancelled');
            request.cancelled?.();
            return false;
        });
        for (const [controller, request] of this.active) {
            if (!controller.signal.aborted && obsolete(request)) {
                controller.abort();
                request.cancelled?.();
            }
        }
    }

    reset(): void {
        this.generation++;
        for (const request of this.queued) {
            request.trace?.finish('cancelled');
            request.cancelled?.();
        }
        this.queued = [];
        for (const [controller, request] of this.active) {
            if (!controller.signal.aborted) {
                controller.abort();
                request.cancelled?.();
            }
        }
    }

    playbackPending(pending: boolean): void {
        this.playing = pending;
        this.drain();
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
        while (!this.playing && this.active.size < this.concurrency && this.queued.length) {
            const request = this.queued.shift()!;
            if (!this.current(request)) {
                request.trace?.finish('stale');
                continue;
            }
            request.trace?.finish('started');
            const controller = new AbortController();
            this.active.set(controller, request);
            void this.transport
                .preview(request.sessionId, request.objectId, request.bins, {
                    signal: controller.signal,
                    readIntent: request.intent,
                })
                .then((preview) => {
                    if (!this.current(request) || controller.signal.aborted) return;
                    if (!sameAudioIdentity(request.identity, preview) || preview.objectId !== request.objectId)
                        throw new Error('Waveform belongs to a different native document');
                    request.loaded(preview);
                })
                .catch((error: unknown) => {
                    if (!this.current(request) || controller.signal.aborted) return;
                    if (error instanceof AxklibApiError && error.code === 'read_preempted')
                        this.queued.unshift({ ...request, trace: undefined });
                    else request.failed(error);
                })
                .finally(() => {
                    this.active.delete(controller);
                    this.drain();
                });
        }
    }
}
