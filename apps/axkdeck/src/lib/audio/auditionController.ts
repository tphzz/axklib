import { audioDiagnosticsEnabled, reportDiagnostic, type DiagnosticLevel } from '../diagnostics';
import type { AuditionDescriptor, ImageTransport } from '../transport';
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
    | { status: 'failed'; playedCount: 0; skippedCount: number; error: string; failedObjectId: string }
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

interface CachedAudition {
    key: string;
    sessionId: number;
    objectId: string;
    descriptor: AuditionDescriptor;
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
    timelineDescriptor: AuditionDescriptor;
    animationFrame?: number;
}

interface ScheduledSequenceSegment {
    entry: CachedAudition;
    source: AudioBufferSourceNode;
    startTime: number;
    endTime: number;
    startFrame: number;
    timelineDescriptor: AuditionDescriptor;
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
const defaultMaximumCacheEntries = 8;
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
        void this.prepareAndStartSequence(sessionId, objectIds, sequenceGeneration);
    }

    private async prepareAndStartSequence(
        sessionId: number,
        objectIds: readonly string[],
        sequenceGeneration: number,
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
            objectId: firstObjectId,
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
            const entries: CachedAudition[] = [];
            let retainedDecodedBytes = 0;
            for (const objectId of objectIds) {
                if (generation !== this.generation || sequenceGeneration !== this.sequenceGeneration) return;
                failedObjectId = objectId;
                const requestKey = this.cacheKey(sessionId, objectId);
                this.activeRequestKey = requestKey;
                const entry = await this.loadAudition(sessionId, objectId, context, false, run, {
                    retainedDecodedBytes,
                    maximumBytes: this.maximumSequenceWorkingSetBytes,
                    overflowMessage: 'Sample Bank audio is too large to audition safely',
                });
                if (generation !== this.generation || sequenceGeneration !== this.sequenceGeneration) return;
                if (!entry) throw new Error('Sample Bank member audio could not be decoded');
                if (retainedDecodedBytes + entry.weightBytes > this.maximumSequenceWorkingSetBytes) {
                    throw new Error('Sample Bank audio is too large to audition safely');
                }
                retainedDecodedBytes += entry.weightBytes;
                entries.push(entry);
            }
            this.activeRequestKey = undefined;
            if (generation !== this.generation || sequenceGeneration !== this.sequenceGeneration) return;
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
            const message = userFacingMessage(error);
            this.emit(run, 'sequence_failed', { message, failedObjectId }, 'error');
            this.releaseSequence('failed');
            this.run = undefined;
            await this.retireOutputContext('failed', run);
            this.update({ objectId: failedObjectId, status: 'failed', playheadFrame: 0, error: message });
            this.settleSequence(sequenceGeneration, {
                status: 'failed',
                playedCount: 0,
                skippedCount: objectIds.length,
                error: message,
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
        pending.promise = this.fetchAndDecode(
            sessionId,
            objectId,
            key,
            context,
            pending,
            run,
            workingSetPolicy,
        ).finally(() => {
            if (this.pending.get(key) === pending) this.pending.delete(key);
        });
        this.pending.set(key, pending);
        if (run) this.emit(run, 'audio_cache_miss');
        return pending.promise;
    }

    private async fetchAndDecode(
        sessionId: number,
        objectId: string,
        key: string,
        context: BaseAudioContext | undefined,
        pending: PendingAudition,
        run?: PlaybackRun,
        workingSetPolicy?: WorkingSetPolicy,
    ): Promise<CachedAudition | null> {
        let auditionId: string | undefined;
        try {
            const prepareStarted = monotonicNow();
            const descriptor = await this.transport.prepareAudition(sessionId, objectId);
            auditionId = descriptor.auditionId;
            if (pending.abort.signal.aborted) throw abortError();
            this.validateDescriptor(descriptor);
            if (run) {
                this.emit(run, 'audition_prepared', {
                    preparationDurationMs: Math.round(monotonicNow() - prepareStarted),
                    auditionId: descriptor.auditionId,
                    sourceSampleRate: descriptor.sampleRate,
                    channels: descriptor.channels,
                    sampleWidthBytes: descriptor.sampleWidthBytes,
                    frameCount: descriptor.frameCount,
                    durationSeconds: descriptor.frameCount / descriptor.sampleRate,
                    loopMode: descriptor.loopMode,
                    loopStartFrame: descriptor.loopStartFrame,
                    loopLengthFrames: descriptor.loopLengthFrames,
                    warningCount: descriptor.warnings.length,
                });
            }

            const decoder = context ?? new OfflineAudioContext(descriptor.channels, 1, descriptor.sampleRate);
            const estimatedBytes = this.estimatedDecodedBytes(descriptor, decoder.sampleRate);
            const retainedDecodedBytes = workingSetPolicy?.retainedDecodedBytes ?? 0;
            const maximumBytes = workingSetPolicy?.maximumBytes ?? this.maximumWorkingSetBytes;
            const workingSetBytes = retainedDecodedBytes + descriptor.wavSizeBytes + estimatedBytes;
            if (!Number.isSafeInteger(workingSetBytes) || workingSetBytes > maximumBytes) {
                throw new Error(workingSetPolicy?.overflowMessage ?? 'Audio is too large to audition safely');
            }
            if (pending.speculative && estimatedBytes > this.cacheBudgetBytes) {
                if (run) this.emit(run, 'audio_prefetch_skipped', { estimatedDecodedBytes: estimatedBytes });
                return null;
            }

            const fetchStarted = monotonicNow();
            const wav = await this.transport.readAuditionAudio(
                descriptor.auditionId,
                descriptor.wavSizeBytes,
                pending.abort.signal,
            );
            if (pending.abort.signal.aborted) throw abortError();
            if (run) {
                this.emit(run, 'audio_fetch_completed', {
                    byteCount: wav.byteLength,
                    fetchDurationMs: Math.round(monotonicNow() - fetchStarted),
                });
            }

            const decodeStarted = monotonicNow();
            const buffer = await decoder.decodeAudioData(wav);
            if (pending.abort.signal.aborted) throw abortError();
            if (buffer.numberOfChannels !== descriptor.channels) {
                throw new Error(
                    `Decoded audio has ${buffer.numberOfChannels} channels; expected ${descriptor.channels}`,
                );
            }
            if (isReversePlayback(descriptor)) {
                for (let channel = 0; channel < buffer.numberOfChannels; channel += 1) {
                    buffer.getChannelData(channel).reverse();
                }
            }
            const weightBytes = buffer.length * buffer.numberOfChannels * Float32Array.BYTES_PER_ELEMENT;
            const entry: CachedAudition = {
                key,
                sessionId,
                objectId,
                descriptor,
                buffer,
                weightBytes,
                transient: weightBytes > this.cacheBudgetBytes || this.maximumCacheEntries === 0,
            };
            if (run) {
                this.emit(run, 'audio_decode_completed', {
                    decodeDurationMs: Math.round(monotonicNow() - decodeStarted),
                    decodedFrames: buffer.length,
                    decodedChannels: buffer.numberOfChannels,
                    decodedBytes: weightBytes,
                    transient: entry.transient,
                });
            }
            if (!entry.transient) this.addCacheEntry(entry);
            return entry;
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
        if (!this.activeRequestKey) return;
        if (this.activeRequestKey === replacementKey) return;
        this.pending.get(this.activeRequestKey)?.abort.abort();
        this.activeRequestKey = undefined;
    }

    private validateDescriptor(descriptor: AuditionDescriptor): void {
        if (!Number.isFinite(descriptor.sampleRate) || descriptor.sampleRate <= 0) {
            throw new Error('Audio sample rate is invalid');
        }
        if (!Number.isInteger(descriptor.channels) || descriptor.channels <= 0) {
            throw new Error('Audio channel count is invalid');
        }
        if (!Number.isInteger(descriptor.frameCount) || descriptor.frameCount <= 0) {
            throw new Error('Audio frame count is invalid');
        }
        if (!Number.isInteger(descriptor.wavSizeBytes) || descriptor.wavSizeBytes <= 44) {
            throw new Error('Audio WAV size is invalid');
        }
    }

    private estimatedDecodedBytes(descriptor: AuditionDescriptor, outputSampleRate: number): number {
        const outputFrames = Math.ceil((descriptor.frameCount * outputSampleRate) / descriptor.sampleRate);
        return outputFrames * descriptor.channels * Float32Array.BYTES_PER_ELEMENT;
    }

    private cacheKey(sessionId: number, objectId: string): string {
        return `${sessionId}:${objectId}`;
    }

    private async fail(error: unknown, objectId: string, run: PlaybackRun): Promise<void> {
        this.generation += 1;
        this.activeRequestKey = undefined;
        const message = userFacingMessage(error);
        this.emit(run, 'playback_failed', { message }, 'error');
        this.run = undefined;
        await this.retireOutputContext('failed', run);
        this.update({ objectId, status: 'failed', playheadFrame: 0, error: message });
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
