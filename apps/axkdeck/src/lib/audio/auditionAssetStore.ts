import type {
    AuditionBundleDescriptor,
    AuditionClipDescriptor,
    AuditionLaneDescriptor,
    ImageTransport,
} from '../transport';
import { isReversePlayback } from './playbackTimeline';
import type {
    AuditionAssetEventSink,
    AuditionControllerOptions,
    CachedAudition,
    PlaybackDescriptor,
    PlaybackRun,
} from './auditionTypes';

interface PendingAudition {
    speculative: boolean;
    abort: AbortController;
    promise: Promise<CachedAudition | null>;
}

interface WorkingSetPolicy {
    retainedDecodedBytes: number;
    maximumBytes: number;
    overflowMessage: string;
}

const defaultCacheBudgetBytes = 128 * 1024 * 1024;
const defaultMaximumWorkingSetBytes = 128 * 1024 * 1024;
const defaultMaximumCacheEntries = 256;

function monotonicNow(): number {
    return globalThis.performance?.now() ?? Date.now();
}

export function abortError(): Error {
    if (typeof DOMException !== 'undefined') return new DOMException('The audio request was cancelled', 'AbortError');
    const error = new Error('The audio request was cancelled');
    error.name = 'AbortError';
    return error;
}

export function isAbortError(error: unknown): boolean {
    return error instanceof Error && error.name === 'AbortError';
}

export class AuditionAssetStore {
    private readonly cacheBudgetBytes: number;
    private readonly maximumCacheEntries: number;
    private readonly maximumWorkingSetBytes: number;
    private readonly maximumSequenceWorkingSetBytes: number;
    private readonly cache = new Map<string, CachedAudition>();
    private readonly pending = new Map<string, PendingAudition>();
    private activeRequestKey?: string;
    private activeBundleAbort?: AbortController;
    private cacheBytes = 0;

    constructor(
        private readonly transport: ImageTransport,
        private readonly emit: AuditionAssetEventSink,
        options: AuditionControllerOptions,
    ) {
        this.cacheBudgetBytes = Math.max(0, options.cacheBudgetBytes ?? defaultCacheBudgetBytes);
        this.maximumCacheEntries = Math.max(0, options.maximumCacheEntries ?? defaultMaximumCacheEntries);
        this.maximumWorkingSetBytes = Math.max(0, options.maximumWorkingSetBytes ?? defaultMaximumWorkingSetBytes);
        this.maximumSequenceWorkingSetBytes = Math.max(
            0,
            options.maximumSequenceWorkingSetBytes ?? this.maximumWorkingSetBytes,
        );
    }

    key(sessionId: number, objectId: string): string {
        return `${sessionId}:${objectId}`;
    }

    beginRequest(key: string): void {
        this.activeRequestKey = key;
    }

    finishRequest(): void {
        this.activeRequestKey = undefined;
        this.activeBundleAbort = undefined;
    }

    hasActiveRequestForSession(sessionId: number): boolean {
        return this.activeRequestKey?.startsWith(`${sessionId}:`) ?? false;
    }

    cancelSpeculativeExcept(key: string): void {
        for (const [pendingKey, pending] of this.pending) {
            if (pendingKey !== key && pending.speculative) pending.abort.abort();
        }
    }

    cancelActiveRequest(replacementKey?: string): void {
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

    load(
        sessionId: number,
        objectId: string,
        context: BaseAudioContext | undefined,
        speculative: boolean,
        run?: PlaybackRun,
        workingSetPolicy?: WorkingSetPolicy,
    ): Promise<CachedAudition | null> {
        const key = this.key(sessionId, objectId);
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

    async loadSequence(
        sessionId: number,
        objectIds: readonly string[],
        context: BaseAudioContext,
        run: PlaybackRun,
    ): Promise<CachedAudition[]> {
        const byId = new Map<string, CachedAudition>();
        const missing: string[] = [];
        for (const objectId of objectIds) {
            const key = this.key(sessionId, objectId);
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

    async invalidateSession(sessionId: number): Promise<void> {
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
        const pending = [...this.pending.values()];
        for (const item of pending) item.abort.abort();
        await Promise.allSettled(pending.map((item) => item.promise));
        this.cache.clear();
        this.cacheBytes = 0;
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
            const decoderSampleRate = context?.sampleRate ?? maximumSourceRate;
            const estimatedBytes = bundle.clips.reduce(
                (total, clip) => total + this.estimatedClipBytes(clip, decoderSampleRate),
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
            const decoderFrameCount = Math.max(
                ...bundle.clips.flatMap((clip) =>
                    clip.lanes.map((lane) => this.resampledFrameCount(lane, decoderSampleRate)),
                ),
            );
            if (!Number.isSafeInteger(decoderFrameCount) || decoderFrameCount <= 0 || decoderFrameCount > 0xffffffff) {
                throw new Error('Audio requires an unsupported decoded frame count');
            }
            const decoder = context ?? new OfflineAudioContext(maximumChannels, decoderFrameCount, decoderSampleRate);

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
                        const minimumFrames = Math.max(
                            1,
                            Math.floor((lane.frameCount * decoder.sampleRate) / lane.sampleRate),
                        );
                        if (speculative && decoded.length < minimumFrames) {
                            throw new Error(
                                `Decoded ${lane.role} Wave Data is truncated (${decoded.length} of ${minimumFrames} frames)`,
                            );
                        }
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
                    frameCount: firstLane.frameCount,
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
                    key: this.key(sessionId, clip.objectId),
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
        const outputFrames = Math.max(...clip.lanes.map((lane) => this.resampledFrameCount(lane, outputSampleRate)));
        return outputFrames * clip.lanes.length * Float32Array.BYTES_PER_ELEMENT;
    }

    private resampledFrameCount(lane: AuditionLaneDescriptor, outputSampleRate: number): number {
        return Math.ceil((lane.frameCount * outputSampleRate) / lane.sampleRate);
    }
}
