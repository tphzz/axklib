import type { ImageTransport } from '../../lib/transport';
import { loadEditorAudio } from '../object-editor/audioSource';

const limit = 128 * 1024 * 1024;
export class MappingPcmCache {
    private scope = '';
    private bytes = 0;
    private readonly buffers = new Map<string, { buffer: AudioBuffer; sourceKey: string; release: () => void }>();
    private readonly pending = new Map<string, { task: Promise<AudioBuffer>; signal: AbortSignal }>();
    constructor(private readonly transport: ImageTransport) {}
    reserve(bytes: number): () => void {
        if (!Number.isSafeInteger(bytes) || bytes < 0 || this.bytes + bytes > limit)
            throw new Error('Matching mappings exceed the 128 MiB preview working limit; no partial group was played.');
        this.bytes += bytes;
        let released = false;
        return () => {
            if (!released) {
                released = true;
                this.bytes -= bytes;
            }
        };
    }
    reset(scope = ''): void {
        if (scope === this.scope) return;
        this.scope = scope;
        this.buffers.forEach(({ release }) => release());
        this.buffers.clear();
        this.pending.clear();
    }
    retain(sessionId: number, revision: number, keys: Set<string>): void {
        this.reset(`${sessionId}:${revision}`);
        for (const [key, entry] of this.buffers) {
            if (!keys.has(entry.sourceKey)) {
                entry.release();
                this.buffers.delete(key);
            }
        }
    }
    async load(
        sessionId: number,
        revision: number,
        id: string,
        context: AudioContext,
        signal: AbortSignal,
        sourceKey = id,
    ): Promise<AudioBuffer> {
        this.reset(`${sessionId}:${revision}`);
        signal.throwIfAborted();
        const buffer = this.buffers.get(sourceKey);
        if (buffer) return buffer.buffer;
        const pending = this.pending.get(sourceKey);
        if (pending && !pending.signal.aborted) return pending.task;
        const scope = this.scope;
        const task = (async () => {
            const audio = await loadEditorAudio(this.transport, sessionId, id, context, signal, this);
            signal.throwIfAborted();
            const bytes = audio.frames * audio.lanes.length * 4;
            const release = this.reserve(bytes * 2);
            try {
                const combined = context.createBuffer(audio.lanes.length, audio.frames, audio.sampleRate);
                audio.lanes.forEach((lane, channel) =>
                    combined.copyToChannel(lane.getChannelData(0).subarray(0, audio.frames), channel),
                );
                if (scope !== this.scope) throw new DOMException('Image changed', 'AbortError');
                release();
                this.buffers.set(sourceKey, { buffer: combined, sourceKey, release: this.reserve(bytes) });
                return combined;
            } catch (error) {
                release();
                throw error;
            }
        })();
        this.pending.set(sourceKey, { task, signal });
        try {
            return await task;
        } finally {
            if (this.pending.get(sourceKey)?.task === task) this.pending.delete(sourceKey);
        }
    }
    reverse(buffer: AudioBuffer, start: number, length: number, context: AudioContext): AudioBuffer {
        const sourceKey = [...this.buffers].find(([, candidate]) => candidate.buffer === buffer)?.[1].sourceKey;
        if (!sourceKey) throw new Error('Reversed preview requires current source PCM.');
        const key = `reverse:${sourceKey}:${start}:${length}`;
        const existing = this.buffers.get(key);
        if (existing) return existing.buffer;
        const release = this.reserve(length * buffer.numberOfChannels * 4);
        try {
            const result = context.createBuffer(buffer.numberOfChannels, length, buffer.sampleRate);
            for (let channel = 0; channel < buffer.numberOfChannels; channel++) {
                const source = buffer.getChannelData(channel),
                    target = result.getChannelData(channel);
                for (let index = 0; index < length; index++) target[index] = source[start + length - index - 1]!;
            }
            this.buffers.set(key, { buffer: result, sourceKey, release });
            return result;
        } catch (error) {
            release();
            throw error;
        }
    }
}
