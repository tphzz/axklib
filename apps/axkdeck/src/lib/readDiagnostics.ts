import { diagnosticsEnabled, reportDiagnostic } from './diagnostics';
import { sameAudioIdentity } from './audio/audioIdentity';
import type { AudioIdentity, ImageTransport, PreviewEnvelope } from './transport';

export interface ReadContext extends AudioIdentity {
    readonly selectionId: number;
    readonly sessionId: number;
    readonly objectId: string;
    readonly started: number;
}

let nextSelection = 0;
let selected: ReadContext | undefined;
let drawnBins = 0;

export function beginSampleRead(
    transport: ImageTransport,
    sessionId: number | null,
    objectId: string,
    fields: Readonly<Record<string, unknown>> = {},
): void {
    selected = undefined;
    drawnBins = 0;
    if (sessionId === null || !diagnosticsEnabled()) return;
    try {
        selected = Object.freeze({
            ...transport.audioIdentity(sessionId),
            sessionId,
            objectId,
            selectionId: ++nextSelection,
            started: performance.now(),
        });
        readTrace('selection', fields, selected)?.finish('started');
    } catch {
        // Capture must not interfere with selection or closed-session handling.
    }
}

export function clearSampleRead(): void {
    selected = undefined;
}

export function readContext(
    sessionId: number | null,
    objectId: string,
    identity?: Pick<AudioIdentity, 'imageId' | 'documentId'> & { revision?: number },
): ReadContext | undefined {
    if (
        !diagnosticsEnabled() ||
        !selected ||
        selected.objectId !== objectId ||
        (sessionId !== null && selected.sessionId !== sessionId) ||
        (identity &&
            (identity.imageId !== selected.imageId ||
                identity.documentId !== selected.documentId ||
                (identity.revision !== undefined && identity.revision !== selected.revision)))
    )
        return undefined;
    return selected;
}

export class ReadTrace {
    private readonly started = performance.now();
    private previous = this.started;
    private ended = false;
    constructor(
        private readonly operation: string,
        private readonly context: ReadContext | undefined,
        private fields: Readonly<Record<string, unknown>>,
    ) {}

    mark(phase: string, fields: Readonly<Record<string, unknown>> = {}): void {
        if (this.ended) return;
        const now = performance.now();
        this.fields = { ...this.fields, ...fields };
        this.emit(`${this.operation}/${phase}`, now, now - this.previous, 'completed');
        this.previous = now;
    }

    finish(outcome: string, fields: Readonly<Record<string, unknown>> = {}): void {
        if (this.ended) return;
        this.ended = true;
        this.fields = { ...this.fields, ...fields };
        const now = performance.now();
        this.emit(this.operation, now, now - this.started, outcome);
    }

    private emit(phase: string, now: number, duration: number, outcome: string): void {
        try {
            const context = this.context;
            reportDiagnostic(
                'sample_read_frontend',
                {
                    ...this.fields,
                    ...(context
                        ? {
                              selectionId: context.selectionId,
                              sessionId: context.sessionId,
                              objectId: context.objectId,
                              imageId: context.imageId,
                              documentId: context.documentId,
                              revision: context.revision,
                              sinceSelectionMs: Math.round((now - context.started) * 1000) / 1000,
                              current: selected === context,
                          }
                        : {}),
                    phase,
                    outcome,
                    durationMs: Math.round(duration * 1000) / 1000,
                },
                'debug',
            );
        } catch {
            // Diagnostics never determine whether a read or playback succeeds.
        }
    }
}

export function readTrace(
    operation: string,
    fields: Readonly<Record<string, unknown>> = {},
    context?: ReadContext,
): ReadTrace | undefined {
    return diagnosticsEnabled() ? new ReadTrace(operation, context, fields) : undefined;
}

export async function finishReadTrace<T>(trace: ReadTrace | undefined, action: () => Promise<T>): Promise<T> {
    try {
        const result = await action();
        trace?.finish('success');
        return result;
    } catch (error) {
        const cancelled =
            (error instanceof Error || (typeof DOMException !== 'undefined' && error instanceof DOMException)) &&
            error.name === 'AbortError';
        trace?.finish(cancelled ? 'cancelled' : 'failed');
        throw error;
    }
}

export interface WaveformReadDraw {
    bins: number;
    width: number;
    height: number;
}

export function reportWaveformReadDraw(
    sessionId: number,
    objectId: string,
    envelope: PreviewEnvelope | null | undefined,
    draw: WaveformReadDraw,
): void {
    const context = readContext(sessionId, objectId);
    if (
        !context ||
        !envelope ||
        envelope.objectId !== objectId ||
        !sameAudioIdentity(context, envelope) ||
        draw.bins <= drawnBins ||
        draw.width <= 1 ||
        draw.height <= 1
    )
        return;
    const phase = drawnBins ? 'waveform-refinement-draw' : 'waveform-first-draw';
    drawnBins = draw.bins;
    readTrace(phase, { ...draw }, context)?.finish('drawn');
}
