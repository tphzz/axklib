import { audioDiagnosticsEnabled, reportDiagnostic, type DiagnosticLevel } from '../diagnostics';
import { AxklibApiError } from '../httpApiClient';
import type {
    AuditionBundleDescriptor,
    AuditionClipDescriptor,
    AuditionLaneDescriptor,
    ImageTransport,
} from '../transport';
import { userFacingMessage } from '../userFacingMessage';
import {
    initialPlaybackFrame,
    isForwardLoop,
    isReversePlayback,
    playbackFrameAtTime,
    playbackOffsetSeconds,
} from './playbackTimeline';

export interface AuditionState {
    objectId: string | null;
    status: 'idle' | 'preparing' | 'playing' | 'failed';
    playheadFrame: number;
    error?: string;
    errorCode?: string;
    errorContext?: unknown;
}

export interface AuditionDiagnosticEvent extends Record<string, unknown> {
    event: string;
    level: DiagnosticLevel;
    playbackId: string;
    objectId: string;
    elapsedMs: number;
}

export type AuditionSequenceResult =
    | { status: 'completed'; playedCount: number; skippedCount: 0 }
    | {
          status: 'failed';
          playedCount: 0;
          skippedCount: number;
          error: string;
          errorCode?: string;
          errorContext?: unknown;
          failedObjectId: string;
      }
    | { status: 'cancelled'; playedCount: 0; skippedCount: number };

interface AuditionControllerOptions {
    cacheBudgetBytes?: number;
    maximumCacheEntries?: number;
    maximumWorkingSetBytes?: number;
    maximumSequenceWorkingSetBytes?: number;
}

interface PlaybackRun {
    id: string;
    objectId: string;
    startedAt: number;
    diagnosticsEnabled: boolean;
    oneShot: boolean;
}

interface PlaybackDescriptor {
    objectId: string;
    sampleRate: number;
    channels: number;
    sampleWidthBytes: number;
    frameCount: number;
    wavSizeBytes: number;
    loopMode: number;
    loopModeLabel: string;
    loopStartFrame: number;
    loopLengthFrames: number;
    warnings: string[];
}

interface CachedAudition {
    key: string;
    sessionId: number;
    objectId: string;
    descriptor: PlaybackDescriptor;
    buffer: AudioBuffer;
    weightBytes: number;
    transient: boolean;
}

interface PendingAudition {
    speculative: boolean;
    abort: AbortController;
    promise: Promise<CachedAudition | null>;
}

interface ActivePlayback {
    entry: CachedAudition;
    source: AudioBufferSourceNode;
    gain: GainNode;
    startFrame: number;
    startTime: number;
    timelineDescriptor: PlaybackDescriptor;
    animationFrame?: number;
}

interface ScheduledSequenceSegment {
    entry: CachedAudition;
    source: AudioBufferSourceNode;
    startTime: number;
    endTime: number;
    startFrame: number;
    timelineDescriptor: PlaybackDescriptor;
}

interface ActiveSequence {
    segments: ScheduledSequenceSegment[];
    gain: GainNode;
    completionGeneration: number;
    animationFrame?: number;
    displayedObjectId?: string;
}

interface SequenceCompletion {
    generation: number;
    memberCount: number;
    oncomplete: (result: AuditionSequenceResult) => void;
}

interface WorkingSetPolicy {
    retainedDecodedBytes: number;
    maximumBytes: number;
    overflowMessage: string;
}

type AuditionDiagnosticSink = (event: AuditionDiagnosticEvent) => void;

const defaultCacheBudgetBytes = 128 * 1024 * 1024;
const defaultMaximumWorkingSetBytes = 128 * 1024 * 1024;
const defaultMaximumCacheEntries = 256;
const startLeadSeconds = 0.01;
const fadeSeconds = 0.005;
const diagnosticSampleBudget = 32_768;

function monotonicNow(): number {
    return globalThis.performance?.now() ?? Date.now();
}

function newPlaybackId(): string {
    return globalThis.crypto?.randomUUID?.() ?? `playback-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function defaultDiagnosticSink({ event, level, ...fields }: AuditionDiagnosticEvent): void {
    reportDiagnostic(event, fields, level);
}

function abortError(): Error {
    if (typeof DOMException !== 'undefined') return new DOMException('The audio request was cancelled', 'AbortError');
    const error = new Error('The audio request was cancelled');
    error.name = 'AbortError';
    return error;
}

function isAbortError(error: unknown): boolean {
    return error instanceof Error && error.name === 'AbortError';
}

function bufferLevelSummary(buffer: AudioBuffer): { peak: number; rms: number; sampledValues: number } {
    const valuesPerChannel = Math.max(1, Math.floor(diagnosticSampleBudget / buffer.numberOfChannels));
    const stride = Math.max(1, Math.floor(buffer.length / valuesPerChannel));
    let peak = 0;
    let squareSum = 0;
    let sampledValues = 0;
    for (let channel = 0; channel < buffer.numberOfChannels; channel += 1) {
        const values = buffer.getChannelData(channel);
        for (let frame = 0; frame < values.length; frame += stride) {
            const value = values[frame] ?? 0;
            peak = Math.max(peak, Math.abs(value));
            squareSum += value * value;
            sampledValues += 1;
        }
    }
    return { peak, rms: sampledValues === 0 ? 0 : Math.sqrt(squareSum / sampledValues), sampledValues };
}

export class AuditionController {
    private readonly cacheBudgetBytes: number;
    private readonly maximumCacheEntries: number;
    private readonly maximumWorkingSetBytes: number;
    private readonly maximumSequenceWorkingSetBytes: number;
    private readonly cache = new Map<string, CachedAudition>();
    private readonly pending = new Map<string, PendingAudition>();
    private readonly closingContexts = new Set<Promise<void>>();
    private context?: AudioContext;
    private active?: ActivePlayback;
    private sequence?: ActiveSequence;
    private sequenceCompletion?: SequenceCompletion;
    private activeRequestKey?: string;
    private activeBundleAbort?: AbortController;
    private cacheBytes = 0;
    private generation = 0;
    private sequenceGeneration = 0;
    private run?: PlaybackRun;

    constructor(
        private readonly transport: ImageTransport,
        private readonly update: (state: AuditionState) => void,
        private readonly diagnostic: AuditionDiagnosticSink = defaultDiagnosticSink,
        private readonly detailedDiagnosticsEnabled: () => boolean = audioDiagnosticsEnabled,
        options: AuditionControllerOptions = {},
    ) {
        this.cacheBudgetBytes = Math.max(0, options.cacheBudgetBytes ?? defaultCacheBudgetBytes);
        this.maximumCacheEntries = Math.max(0, options.maximumCacheEntries ?? defaultMaximumCacheEntries);
        this.maximumWorkingSetBytes = Math.max(0, options.maximumWorkingSetBytes ?? defaultMaximumWorkingSetBytes);
        this.maximumSequenceWorkingSetBytes = Math.max(
            0,
            options.maximumSequenceWorkingSetBytes ?? this.maximumWorkingSetBytes,
        );
    }

    async prefetch(sessionId: number, objectId: string): Promise<void> {
        const key = this.cacheKey(sessionId, objectId);
        for (const [pendingKey, pending] of this.pending) {
            if (pendingKey !== key && pending.speculative) pending.abort.abort();
        }
        try {
            await this.loadAudition(sessionId, objectId, undefined, true);
        } catch (error) {
            if (!isAbortError(error) && this.detailedDiagnosticsEnabled()) {
                reportDiagnostic('audio_prefetch_failed', { objectId, message: String(error) }, 'warn');
            }
        }
    }

    async play(sessionId: number, objectId: string): Promise<void> {
        this.sequenceGeneration += 1;
        this.cancelSequenceCompletion();
        await this.playOne(sessionId, objectId);
    }

    playSequence(
        sessionId: number,
        objectIds: readonly string[],
        oncomplete: (result: AuditionSequenceResult) => void = () => undefined,
        sequenceObjectId?: string,
    ): void {
        const sequenceGeneration = ++this.sequenceGeneration;
        this.cancelSequenceCompletion();
        this.sequenceCompletion = { generation: sequenceGeneration, memberCount: objectIds.length, oncomplete };
        if (objectIds.length === 0) {
            this.generation += 1;
            this.cancelActiveRequest();
            const run = this.run;
            this.releaseActive('replaced');
            this.releaseSequence('replaced');
            this.run = undefined;
            this.update({ objectId: null, status: 'idle', playheadFrame: 0 });
            void this.retireOutputContext('replaced', run);
            this.settleSequence(sequenceGeneration, { status: 'completed', playedCount: 0, skippedCount: 0 });
            return;
        }
        void this.prepareAndStartSequence(sessionId, objectIds, sequenceGeneration, sequenceObjectId);
    }

    private async prepareAndStartSequence(
        sessionId: number,
        objectIds: readonly string[],
        sequenceGeneration: number,
        sequenceObjectId?: string,
    ): Promise<void> {
        const generation = ++this.generation;
        this.cancelActiveRequest();
        const previousRun = this.run;
        this.releaseActive('replaced');
        this.releaseSequence('replaced');
        const previousContextClosed = this.retireOutputContext('replaced', previousRun);
        const firstObjectId = objectIds[0]!;
        const run: PlaybackRun = {
            id: newPlaybackId(),
            objectId: sequenceObjectId ?? firstObjectId,
            startedAt: monotonicNow(),
            diagnosticsEnabled: this.detailedDiagnosticsEnabled(),
            oneShot: true,
        };
        this.run = run;
        this.emit(run, 'sequence_requested', { sessionId, memberCount: objectIds.length });
        this.update({ objectId: firstObjectId, status: 'preparing', playheadFrame: 0 });

        let failedObjectId = firstObjectId;
        try {
            const context = this.ensureContext();
            const resumed = this.resumeContext(context, run);
            await Promise.all([previousContextClosed, resumed]);
            const entries = await this.loadAuditionSequence(sessionId, objectIds, context, run);
            this.activeRequestKey = undefined;
            this.activeBundleAbort = undefined;
            if (generation !== this.generation || sequenceGeneration !== this.sequenceGeneration) return;
            const retainedDecodedBytes = entries.reduce((total, entry) => total + entry.weightBytes, 0);
            this.emit(run, 'sequence_prepared', {
                memberCount: entries.length,
                decodedBytes: retainedDecodedBytes,
                preparationDurationMs: Math.round(monotonicNow() - run.startedAt),
            });
            this.startSequence(entries, run, context, sequenceGeneration);
        } catch (error) {
            if (
                generation !== this.generation ||
                sequenceGeneration !== this.sequenceGeneration ||
                isAbortError(error)
            ) {
                return;
            }
            this.activeRequestKey = undefined;
            this.activeBundleAbort = undefined;
            const message = userFacingMessage(error);
            const typed = this.typedError(error);
            if (typed.errorContext && typeof typed.errorContext === 'object' && 'objectId' in typed.errorContext) {
                const contextObjectId = (typed.errorContext as { objectId?: unknown }).objectId;
                if (typeof contextObjectId === 'string') failedObjectId = contextObjectId;
            }
            this.emit(run, 'sequence_failed', { message, failedObjectId }, 'error');
            this.releaseSequence('failed');
            this.run = undefined;
            await this.retireOutputContext('failed', run);
            this.update({
                objectId: failedObjectId,
                status: 'failed',
                playheadFrame: 0,
                error: message,
                ...typed,
            });
            this.settleSequence(sequenceGeneration, {
                status: 'failed',
                playedCount: 0,
                skippedCount: objectIds.length,
                error: message,
                ...typed,
                failedObjectId,
            });
        }
    }

    private async playOne(sessionId: number, objectId: string): Promise<void> {
        const generation = ++this.generation;
        const requestKey = this.cacheKey(sessionId, objectId);
        this.cancelActiveRequest(requestKey);
        const previousRun = this.run;
        this.releaseActive('replaced');
        this.releaseSequence('replaced');
        const previousContextClosed = this.retireOutputContext('replaced', previousRun);
        const run: PlaybackRun = {
            id: newPlaybackId(),
            objectId,
            startedAt: monotonicNow(),
            diagnosticsEnabled: this.detailedDiagnosticsEnabled(),
            oneShot: false,
        };
        this.run = run;
        this.activeRequestKey = requestKey;
        this.emit(run, 'playback_requested', { sessionId });
        this.update({ objectId, status: 'preparing', playheadFrame: 0 });

        try {
            const context = this.ensureContext();
            // Resume synchronously from the click handler before any network await.
            const resumed = this.resumeContext(context, run);
            const loaded = this.loadAudition(sessionId, objectId, context, false, run);
            let [, , entry] = await Promise.all([previousContextClosed, resumed, loaded]);
            if (generation !== this.generation) return;
            // An oversized speculative request may finish just as an explicit play promotes it.
            if (!entry) entry = await this.loadAudition(sessionId, objectId, context, false, run);
            if (generation !== this.generation || !entry) return;
            this.activeRequestKey = undefined;
            this.startPlayback(entry, initialPlaybackFrame(entry.descriptor), run, context);
        } catch (error) {
            if (generation !== this.generation || isAbortError(error)) return;
            await this.fail(error, objectId, run);
        }
    }

    seek(frame: number): void {
        const active = this.active;
        const run = this.run;
        if (!active || !run) return;
        const clamped = Math.max(0, Math.min(active.entry.descriptor.frameCount - 1, Math.floor(frame)));
        this.emit(run, 'playback_seek_requested', { sourceFrame: clamped });
        this.releaseActive('seek');
        this.startPlayback(active.entry, clamped, run);
    }

    async stop(): Promise<void> {
        this.sequenceGeneration += 1;
        this.generation += 1;
        this.cancelActiveRequest();
        this.cancelSequenceCompletion();
        const run = this.run;
        if (run) this.emit(run, 'playback_stop_requested');
        const hadActivePlayback = Boolean(this.active || this.sequence);
        this.releaseActive('stopped');
        this.releaseSequence('stopped');
        this.run = undefined;
        this.update({ objectId: null, status: 'idle', playheadFrame: 0 });
        await this.retireOutputContext('stopped', run, hadActivePlayback ? fadeSeconds : 0);
    }

    async invalidateSession(sessionId: number): Promise<void> {
        if (
            this.active?.entry.sessionId === sessionId ||
            this.sequence?.segments.some((segment) => segment.entry.sessionId === sessionId) ||
            this.activeRequestKey?.startsWith(`${sessionId}:`)
        ) {
            await this.stop();
        }
        const interrupted: Promise<CachedAudition | null>[] = [];
        for (const [key, pending] of this.pending) {
            if (key.startsWith(`${sessionId}:`)) {
                pending.abort.abort();
                interrupted.push(pending.promise);
            }
        }
        await Promise.allSettled(interrupted);
        for (const [key, entry] of this.cache) {
            if (entry.sessionId === sessionId) this.removeCacheEntry(key, entry);
        }
    }

    async dispose(): Promise<void> {
        await this.stop();
        const pending = [...this.pending.values()];
        for (const item of pending) item.abort.abort();
        await Promise.allSettled(pending.map((item) => item.promise));
        this.cache.clear();
        this.cacheBytes = 0;
        await Promise.allSettled([...this.closingContexts]);
    }

    private ensureContext(): AudioContext {
        if (this.context && this.context.state !== 'closed') return this.context;
        const context = new AudioContext();
        this.context = context;
        return context;
    }

    private async resumeContext(context: AudioContext, run: PlaybackRun): Promise<void> {
        this.emit(run, 'audio_context_ready', {
            state: context.state,
            outputSampleRate: context.sampleRate,
            baseLatencySeconds: context.baseLatency,
            outputLatencySeconds: 'outputLatency' in context ? context.outputLatency : null,
        });
        const started = monotonicNow();
        await context.resume();
        this.emit(run, 'audio_context_resumed', {
            state: context.state,
            resumeDurationMs: Math.round(monotonicNow() - started),
        });
    }

    private loadAudition(
        sessionId: number,
        objectId: string,
        context: BaseAudioContext | undefined,
        speculative: boolean,
        run?: PlaybackRun,
        workingSetPolicy?: WorkingSetPolicy,
    ): Promise<CachedAudition | null> {
        const key = this.cacheKey(sessionId, objectId);
        const cached = this.cache.get(key);
        if (cached) {
            this.cache.delete(key);
            this.cache.set(key, cached);
            if (run) this.emit(run, 'audio_cache_hit', { decodedBytes: cached.weightBytes });
            return Promise.resolve(cached);
        }

        const existing = this.pending.get(key);
        if (existing) {
            if (!speculative) existing.speculative = false;
            if (run) this.emit(run, 'audio_cache_wait', { speculative: existing.speculative });
            return existing.promise;
        }

        const pending: PendingAudition = {
            speculative,
            abort: new AbortController(),
            promise: Promise.resolve(null),
        };
        pending.promise = this.fetchAndDecodeBundle(
            sessionId,
            [objectId],
            context,
            pending.abort.signal,
            pending.speculative,
            run,
            workingSetPolicy,
        )
            .then((entries) => entries.get(objectId) ?? null)
            .finally(() => {
                if (this.pending.get(key) === pending) this.pending.delete(key);
            });
        this.pending.set(key, pending);
        if (run) this.emit(run, 'audio_cache_miss');
        return pending.promise;
    }

    private async loadAuditionSequence(
        sessionId: number,
        objectIds: readonly string[],
        context: BaseAudioContext,
        run: PlaybackRun,
    ): Promise<CachedAudition[]> {
        const byId = new Map<string, CachedAudition>();
        const missing: string[] = [];
        for (const objectId of objectIds) {
            const key = this.cacheKey(sessionId, objectId);
            const cached = this.cache.get(key);
            if (cached) {
                this.cache.delete(key);
                this.cache.set(key, cached);
                byId.set(objectId, cached);
                continue;
            }
            if (!missing.includes(objectId)) missing.push(objectId);
        }
        const retainedDecodedBytes = [...byId.values()].reduce((total, entry) => total + entry.weightBytes, 0);
        if (retainedDecodedBytes > this.maximumSequenceWorkingSetBytes) {
            throw new Error('Sample Bank audio is too large to audition safely');
        }
        if (missing.length > 0) {
            const abort = new AbortController();
            this.activeBundleAbort = abort;
            this.activeRequestKey = `${sessionId}:bundle`;
            this.emit(run, 'audio_bundle_cache_miss', {
                requestedCount: objectIds.length,
                missingCount: missing.length,
            });
            const decoded = await this.fetchAndDecodeBundle(sessionId, missing, context, abort.signal, false, run, {
                retainedDecodedBytes,
                maximumBytes: this.maximumSequenceWorkingSetBytes,
                overflowMessage: 'Sample Bank audio is too large to audition safely',
            });
            for (const [objectId, entry] of decoded) byId.set(objectId, entry);
        } else {
            this.emit(run, 'audio_bundle_cache_hit', { requestedCount: objectIds.length });
        }
        return objectIds.map((objectId) => {
            const entry = byId.get(objectId);
            if (!entry) throw new Error(`Sample Bank member ${objectId} could not be decoded`);
            return entry;
        });
    }

    private async fetchAndDecodeBundle(
        sessionId: number,
        objectIds: readonly string[],
        context: BaseAudioContext | undefined,
        signal: AbortSignal,
        speculative: boolean,
        run?: PlaybackRun,
        workingSetPolicy?: WorkingSetPolicy,
    ): Promise<Map<string, CachedAudition>> {
        let auditionId: string | undefined;
        try {
            const prepareStarted = monotonicNow();
            const bundle = await this.transport.prepareAuditionBundle(sessionId, objectIds, signal);
            auditionId = bundle.auditionId;
            if (signal.aborted) throw abortError();
            this.validateBundle(bundle, objectIds);
            if (run) {
                this.emit(run, 'audition_bundle_prepared', {
                    preparationDurationMs: Math.round(monotonicNow() - prepareStarted),
                    auditionId: bundle.auditionId,
                    clipCount: bundle.clips.length,
                    laneCount: bundle.clips.reduce((total, clip) => total + clip.lanes.length, 0),
                    contentSizeBytes: bundle.contentSizeBytes,
                });
            }

            const maximumSourceRate = Math.max(
                ...bundle.clips.flatMap((clip) => clip.lanes.map((lane) => lane.sampleRate)),
            );
            const maximumChannels = Math.max(...bundle.clips.map((clip) => clip.lanes.length));
            const decoder = context ?? new OfflineAudioContext(maximumChannels, 1, maximumSourceRate);
            const estimatedBytes = bundle.clips.reduce(
                (total, clip) => total + this.estimatedClipBytes(clip, decoder.sampleRate),
                0,
            );
            const retainedDecodedBytes = workingSetPolicy?.retainedDecodedBytes ?? 0;
            const maximumBytes = workingSetPolicy?.maximumBytes ?? this.maximumWorkingSetBytes;
            const workingSetBytes = retainedDecodedBytes + bundle.contentSizeBytes + estimatedBytes;
            if (!Number.isSafeInteger(workingSetBytes) || workingSetBytes > maximumBytes) {
                throw new Error(workingSetPolicy?.overflowMessage ?? 'Audio is too large to audition safely');
            }
            if (speculative && estimatedBytes > this.cacheBudgetBytes) {
                if (run) this.emit(run, 'audio_prefetch_skipped', { estimatedDecodedBytes: estimatedBytes });
                return new Map();
            }

            const fetchStarted = monotonicNow();
            const content = await this.transport.readAuditionContent(
                bundle.auditionId,
                bundle.contentSizeBytes,
                signal,
            );
            if (signal.aborted) throw abortError();
            if (run) {
                this.emit(run, 'audio_bundle_fetch_completed', {
                    byteCount: content.byteLength,
                    fetchDurationMs: Math.round(monotonicNow() - fetchStarted),
                });
            }

            const decodeStarted = monotonicNow();
            const entries = new Map<string, CachedAudition>();
            for (const clip of bundle.clips) {
                const laneBuffers = await Promise.all(
                    clip.lanes.map(async (lane) => {
                        const start = lane.contentOffsetBytes;
                        const end = start + lane.wavSizeBytes;
                        const decoded = await decoder.decodeAudioData(content.slice(start, end));
                        if (decoded.numberOfChannels !== 1) {
                            throw new Error(
                                `Decoded ${lane.role} Wave Data has ${decoded.numberOfChannels} channels; expected 1`,
                            );
                        }
                        return decoded;
                    }),
                );
                if (signal.aborted) throw abortError();
                const frameCount = Math.max(...laneBuffers.map((buffer) => buffer.length));
                const buffer =
                    laneBuffers.length === 1
                        ? laneBuffers[0]!
                        : decoder.createBuffer(laneBuffers.length, frameCount, decoder.sampleRate);
                if (laneBuffers.length > 1) {
                    laneBuffers.forEach((laneBuffer, channel) => {
                        buffer.getChannelData(channel).set(laneBuffer.getChannelData(0));
                    });
                }
                const firstLane = clip.lanes[0]!;
                const descriptor: PlaybackDescriptor = {
                    objectId: clip.objectId,
                    sampleRate: firstLane.sampleRate,
                    channels: laneBuffers.length,
                    sampleWidthBytes: Math.max(...clip.lanes.map((lane) => lane.sampleWidthBytes)),
                    frameCount: Math.round(buffer.duration * firstLane.sampleRate),
                    wavSizeBytes: clip.lanes.reduce((total, lane) => total + lane.wavSizeBytes, 0),
                    loopMode: clip.loopMode,
                    loopModeLabel: clip.loopModeLabel,
                    loopStartFrame: firstLane.loopStartFrame,
                    loopLengthFrames: firstLane.loopLengthFrames,
                    warnings: clip.warnings,
                };
                if (isReversePlayback(descriptor)) {
                    for (let channel = 0; channel < buffer.numberOfChannels; channel += 1) {
                        buffer.getChannelData(channel).reverse();
                    }
                }
                const weightBytes = buffer.length * buffer.numberOfChannels * Float32Array.BYTES_PER_ELEMENT;
                const entry: CachedAudition = {
                    key: this.cacheKey(sessionId, clip.objectId),
                    sessionId,
                    objectId: clip.objectId,
                    descriptor,
                    buffer,
                    weightBytes,
                    transient: weightBytes > this.cacheBudgetBytes || this.maximumCacheEntries === 0,
                };
                if (!entry.transient) this.addCacheEntry(entry);
                entries.set(clip.objectId, entry);
            }
            if (run) {
                this.emit(run, 'audio_bundle_decode_completed', {
                    decodeDurationMs: Math.round(monotonicNow() - decodeStarted),
                    decodedClips: entries.size,
                    decodedBytes: [...entries.values()].reduce((total, entry) => total + entry.weightBytes, 0),
                });
            }
            return entries;
        } finally {
            if (auditionId) await this.transport.deleteAudition(auditionId).catch(() => undefined);
        }
    }

    private startPlayback(entry: CachedAudition, sourceFrame: number, run: PlaybackRun, context = this.context): void {
        if (!context || context !== this.context || context.state === 'closed') {
            throw new Error('Audio output became unavailable before playback started');
        }
        const source = context.createBufferSource();
        const gain = context.createGain();
        source.buffer = entry.buffer;
        if (!run.oneShot && isForwardLoop(entry.descriptor)) {
            source.loop = true;
            source.loopStart = entry.descriptor.loopStartFrame / entry.descriptor.sampleRate;
            source.loopEnd =
                (entry.descriptor.loopStartFrame + entry.descriptor.loopLengthFrames) / entry.descriptor.sampleRate;
        }
        source.connect(gain);
        gain.connect(context.destination);
        const startTime = context.currentTime + startLeadSeconds;
        gain.gain.value = 0;
        gain.gain.setValueAtTime(0, context.currentTime);
        gain.gain.setValueAtTime(0, startTime);
        gain.gain.linearRampToValueAtTime(1, startTime + fadeSeconds);
        const timelineDescriptor =
            run.oneShot && isForwardLoop(entry.descriptor)
                ? { ...entry.descriptor, loopMode: 0, loopStartFrame: 0, loopLengthFrames: 0 }
                : entry.descriptor;
        const active: ActivePlayback = {
            entry,
            source,
            gain,
            startFrame: sourceFrame,
            startTime,
            timelineDescriptor,
        };
        this.active = active;
        source.onended = () => void this.handleEnded(active, run);
        source.start(startTime, playbackOffsetSeconds(entry.descriptor, sourceFrame));
        if (run.diagnosticsEnabled) {
            this.emit(run, 'audio_buffer_levels', {
                ...bufferLevelSummary(entry.buffer),
                decodedFrames: entry.buffer.length,
                decodedChannels: entry.buffer.numberOfChannels,
                decodedSampleRate: entry.buffer.sampleRate,
            });
        }
        this.emit(run, 'playback_scheduled', {
            startLeadMs: startLeadSeconds * 1000,
            attackMs: fadeSeconds * 1000,
            sourceFrame,
            sourceOffsetSeconds: playbackOffsetSeconds(entry.descriptor, sourceFrame),
            loop: source.loop,
        });
        this.update({ objectId: entry.objectId, status: 'playing', playheadFrame: sourceFrame });
        this.scheduleCursor(active);
    }

    private startSequence(
        entries: readonly CachedAudition[],
        run: PlaybackRun,
        context: AudioContext,
        completionGeneration: number,
    ): void {
        if (context !== this.context || context.state === 'closed') {
            throw new Error('Audio output became unavailable before Sample Bank playback started');
        }
        const gain = context.createGain();
        gain.connect(context.destination);
        const sequenceStart = context.currentTime + startLeadSeconds;
        gain.gain.value = 0;
        gain.gain.setValueAtTime(0, context.currentTime);
        gain.gain.setValueAtTime(0, sequenceStart);
        gain.gain.linearRampToValueAtTime(1, sequenceStart + fadeSeconds);

        let memberStart = sequenceStart;
        const segments = entries.map((entry) => {
            if (!Number.isFinite(entry.buffer.duration) || entry.buffer.duration <= 0) {
                throw new Error('Sample Bank member duration is invalid');
            }
            const source = context.createBufferSource();
            source.buffer = entry.buffer;
            source.loop = false;
            source.connect(gain);
            const startFrame = initialPlaybackFrame(entry.descriptor);
            const timelineDescriptor = isForwardLoop(entry.descriptor)
                ? { ...entry.descriptor, loopMode: 0, loopStartFrame: 0, loopLengthFrames: 0 }
                : entry.descriptor;
            const segment: ScheduledSequenceSegment = {
                entry,
                source,
                startTime: memberStart,
                endTime: memberStart + entry.buffer.duration,
                startFrame,
                timelineDescriptor,
            };
            memberStart = segment.endTime;
            return segment;
        });
        const sequence: ActiveSequence = { segments, gain, completionGeneration };
        this.sequence = sequence;
        for (const [index, segment] of segments.entries()) {
            segment.source.onended = () => {
                segment.source.disconnect();
                if (index === segments.length - 1) void this.handleSequenceEnded(sequence, run);
            };
            segment.source.start(segment.startTime);
        }
        this.emit(run, 'sequence_scheduled', {
            memberCount: segments.length,
            startLeadMs: startLeadSeconds * 1000,
            attackMs: fadeSeconds * 1000,
            durationSeconds: memberStart - sequenceStart,
        });
        this.update({ objectId: entries[0]!.objectId, status: 'playing', playheadFrame: segments[0]!.startFrame });
        this.scheduleSequenceCursor(sequence, run);
    }

    private scheduleCursor(active: ActivePlayback): void {
        if (typeof requestAnimationFrame !== 'function') return;
        const tick = () => {
            if (this.active !== active) return;
            const elapsed = Math.max(0, this.audibleContextTime() - active.startTime);
            const frame = playbackFrameAtTime(active.timelineDescriptor, active.startFrame, elapsed);
            if (frame !== null) {
                this.update({ objectId: active.entry.objectId, status: 'playing', playheadFrame: frame });
            }
            active.animationFrame = requestAnimationFrame(tick);
        };
        active.animationFrame = requestAnimationFrame(tick);
    }

    private scheduleSequenceCursor(sequence: ActiveSequence, run: PlaybackRun): void {
        if (typeof requestAnimationFrame !== 'function') return;
        const tick = () => {
            if (this.sequence !== sequence) return;
            const audibleTime = this.audibleContextTime();
            const segment =
                sequence.segments.find((candidate) => audibleTime < candidate.endTime) ?? sequence.segments.at(-1);
            if (segment) {
                const elapsed = Math.max(0, audibleTime - segment.startTime);
                const frame = playbackFrameAtTime(segment.timelineDescriptor, segment.startFrame, elapsed);
                if (frame !== null) {
                    if (sequence.displayedObjectId !== segment.entry.objectId) {
                        sequence.displayedObjectId = segment.entry.objectId;
                        this.emit(run, 'sequence_member_changed', { memberObjectId: segment.entry.objectId });
                    }
                    this.update({ objectId: segment.entry.objectId, status: 'playing', playheadFrame: frame });
                }
            }
            sequence.animationFrame = requestAnimationFrame(tick);
        };
        sequence.animationFrame = requestAnimationFrame(tick);
    }

    private audibleContextTime(): number {
        const context = this.context;
        if (!context) return 0;
        if (typeof context.getOutputTimestamp === 'function') {
            const timestamp = context.getOutputTimestamp();
            const contextTime = timestamp.contextTime;
            const performanceTime = timestamp.performanceTime;
            if (contextTime !== undefined && performanceTime !== undefined && contextTime > 0 && performanceTime > 0) {
                const elapsed = Math.max(0, monotonicNow() - performanceTime) / 1000;
                return Math.min(context.currentTime, contextTime + elapsed);
            }
        }
        const outputLatency = 'outputLatency' in context ? context.outputLatency : 0;
        return Math.max(0, context.currentTime - context.baseLatency - outputLatency);
    }

    private releaseActive(reason: string): void {
        const active = this.active;
        if (!active) return;
        this.active = undefined;
        if (active.animationFrame !== undefined && typeof cancelAnimationFrame === 'function') {
            cancelAnimationFrame(active.animationFrame);
        }
        const context = this.context;
        const stopTime = context ? context.currentTime + fadeSeconds : 0;
        if (context) {
            active.gain.gain.cancelScheduledValues(context.currentTime);
            active.gain.gain.setValueAtTime(active.gain.gain.value, context.currentTime);
            active.gain.gain.linearRampToValueAtTime(0, stopTime);
        }
        active.source.onended = () => {
            active.source.disconnect();
            active.gain.disconnect();
        };
        try {
            active.source.stop(stopTime);
        } catch {
            active.source.disconnect();
            active.gain.disconnect();
        }
        if (this.run) this.emit(this.run, 'playback_released', { reason, transient: active.entry.transient });
    }

    private releaseSequence(reason: string): void {
        const sequence = this.sequence;
        if (!sequence) return;
        this.sequence = undefined;
        if (sequence.animationFrame !== undefined && typeof cancelAnimationFrame === 'function') {
            cancelAnimationFrame(sequence.animationFrame);
        }
        const context = this.context;
        const stopTime = context ? context.currentTime + fadeSeconds : 0;
        if (context) {
            sequence.gain.gain.cancelScheduledValues(context.currentTime);
            sequence.gain.gain.setValueAtTime(sequence.gain.gain.value, context.currentTime);
            sequence.gain.gain.linearRampToValueAtTime(0, stopTime);
        }
        for (const segment of sequence.segments) {
            segment.source.onended = () => segment.source.disconnect();
            try {
                segment.source.stop(stopTime);
            } catch {
                segment.source.disconnect();
            }
        }
        if (this.run) this.emit(this.run, 'sequence_released', { reason, memberCount: sequence.segments.length });
    }

    private async handleEnded(active: ActivePlayback, run: PlaybackRun): Promise<void> {
        active.source.disconnect();
        active.gain.disconnect();
        if (this.active !== active) return;
        this.active = undefined;
        if (active.animationFrame !== undefined && typeof cancelAnimationFrame === 'function') {
            cancelAnimationFrame(active.animationFrame);
        }
        this.emit(run, 'playback_ended');
        this.run = undefined;
        this.update({ objectId: null, status: 'idle', playheadFrame: 0 });
        await this.retireOutputContext('ended', run);
    }

    private async handleSequenceEnded(sequence: ActiveSequence, run: PlaybackRun): Promise<void> {
        if (this.sequence !== sequence) return;
        this.sequence = undefined;
        if (sequence.animationFrame !== undefined && typeof cancelAnimationFrame === 'function') {
            cancelAnimationFrame(sequence.animationFrame);
        }
        for (const segment of sequence.segments) segment.source.disconnect();
        sequence.gain.disconnect();
        this.emit(run, 'sequence_ended', { memberCount: sequence.segments.length });
        this.run = undefined;
        this.update({ objectId: null, status: 'idle', playheadFrame: 0 });
        await this.retireOutputContext('ended', run);
        this.settleSequence(sequence.completionGeneration, {
            status: 'completed',
            playedCount: sequence.segments.length,
            skippedCount: 0,
        });
    }

    private retireOutputContext(reason: string, run?: PlaybackRun, delaySeconds = 0): Promise<void> {
        const context = this.context;
        if (!context) return Promise.resolve();
        this.context = undefined;
        context.onstatechange = null;
        const closing = this.closeOutputContext(context, reason, run, delaySeconds)
            .catch((error) => {
                reportDiagnostic('audio_context_close_failed', { reason, message: userFacingMessage(error) }, 'warn');
            })
            .finally(() => {
                this.closingContexts.delete(closing);
            });
        this.closingContexts.add(closing);
        return closing;
    }

    private async closeOutputContext(
        context: AudioContext,
        reason: string,
        run?: PlaybackRun,
        delaySeconds = 0,
    ): Promise<void> {
        if (delaySeconds > 0) {
            await new Promise<void>((resolve) => window.setTimeout(resolve, delaySeconds * 1000));
        }
        if (context.state !== 'closed') await context.close();
        if (run) this.emit(run, 'audio_context_closed', { reason });
    }

    private addCacheEntry(entry: CachedAudition): void {
        this.cache.set(entry.key, entry);
        this.cacheBytes += entry.weightBytes;
        while (this.cache.size > this.maximumCacheEntries || this.cacheBytes > this.cacheBudgetBytes) {
            const oldest = this.cache.entries().next().value as [string, CachedAudition] | undefined;
            if (!oldest) break;
            this.removeCacheEntry(oldest[0], oldest[1]);
        }
    }

    private removeCacheEntry(key: string, entry: CachedAudition): void {
        if (!this.cache.delete(key)) return;
        this.cacheBytes -= entry.weightBytes;
    }

    private settleSequence(generation: number, result: AuditionSequenceResult): void {
        const completion = this.sequenceCompletion;
        if (!completion || completion.generation !== generation) return;
        this.sequenceCompletion = undefined;
        completion.oncomplete(result);
    }

    private cancelSequenceCompletion(): void {
        const completion = this.sequenceCompletion;
        if (!completion) return;
        this.settleSequence(completion.generation, {
            status: 'cancelled',
            playedCount: 0,
            skippedCount: completion.memberCount,
        });
    }

    private cancelActiveRequest(replacementKey?: string): void {
        if (!this.activeRequestKey) {
            this.activeBundleAbort?.abort();
            this.activeBundleAbort = undefined;
            return;
        }
        if (this.activeRequestKey === replacementKey) return;
        this.pending.get(this.activeRequestKey)?.abort.abort();
        this.activeBundleAbort?.abort();
        this.activeBundleAbort = undefined;
        this.activeRequestKey = undefined;
    }

    private validateBundle(bundle: AuditionBundleDescriptor, requestedObjectIds: readonly string[]): void {
        if (!Number.isSafeInteger(bundle.contentSizeBytes) || bundle.contentSizeBytes <= 0) {
            throw new Error('Audio bundle size is invalid');
        }
        if (bundle.clips.length !== requestedObjectIds.length) {
            throw new Error('Audio bundle did not contain every requested Sample');
        }
        let expectedOffset = 0;
        bundle.clips.forEach((clip, clipIndex) => {
            if (clip.objectId !== requestedObjectIds[clipIndex]) {
                throw new Error('Audio bundle clip order does not match the requested Samples');
            }
            if (clip.lanes.length < 1 || clip.lanes.length > 2) {
                throw new Error('Audio bundle clip must contain one or two Wave Data lanes');
            }
            clip.lanes.forEach((lane) => {
                this.validateLane(lane, expectedOffset);
                expectedOffset += lane.wavSizeBytes;
            });
        });
        if (expectedOffset !== bundle.contentSizeBytes) {
            throw new Error('Audio bundle content layout is inconsistent');
        }
    }

    private validateLane(lane: AuditionLaneDescriptor, expectedOffset: number): void {
        if (!Number.isFinite(lane.sampleRate) || lane.sampleRate <= 0) {
            throw new Error('Audio sample rate is invalid');
        }
        if (!Number.isInteger(lane.frameCount) || lane.frameCount <= 0) {
            throw new Error('Audio frame count is invalid');
        }
        if (!Number.isSafeInteger(lane.wavSizeBytes) || lane.wavSizeBytes <= 44) {
            throw new Error('Audio WAV size is invalid');
        }
        if (lane.contentOffsetBytes !== expectedOffset) {
            throw new Error('Audio bundle lane offsets are inconsistent');
        }
    }

    private estimatedClipBytes(clip: AuditionClipDescriptor, outputSampleRate: number): number {
        const outputFrames = Math.max(
            ...clip.lanes.map((lane) => Math.ceil((lane.frameCount * outputSampleRate) / lane.sampleRate)),
        );
        return outputFrames * clip.lanes.length * Float32Array.BYTES_PER_ELEMENT;
    }

    private cacheKey(sessionId: number, objectId: string): string {
        return `${sessionId}:${objectId}`;
    }

    private async fail(error: unknown, objectId: string, run: PlaybackRun): Promise<void> {
        this.generation += 1;
        this.activeRequestKey = undefined;
        const message = userFacingMessage(error);
        const typed = this.typedError(error);
        this.emit(run, 'playback_failed', { message }, 'error');
        this.run = undefined;
        await this.retireOutputContext('failed', run);
        this.update({ objectId, status: 'failed', playheadFrame: 0, error: message, ...typed });
    }

    private typedError(error: unknown): Pick<AuditionState, 'errorCode' | 'errorContext'> {
        if (!(error instanceof AxklibApiError)) return {};
        return { errorCode: error.code, errorContext: error.context };
    }

    private emit(
        run: PlaybackRun,
        event: string,
        fields: Record<string, unknown> = {},
        level: DiagnosticLevel = 'info',
    ): void {
        if (!run.diagnosticsEnabled && level !== 'error') return;
        this.diagnostic({
            ...fields,
            event,
            level,
            playbackId: run.id,
            objectId: run.objectId,
            elapsedMs: Math.round(monotonicNow() - run.startedAt),
        });
    }
}
