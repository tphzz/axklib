import { describe, expect, it, vi } from 'vitest';
import type { AudioIdentity, ImageTransport, PreviewEnvelope } from '../transport';
import { WaveformPreviewQueue } from './waveformPreviewQueue';

function fixture() {
    let identity: AudioIdentity = { imageId: 'image-1', documentId: 'document-1', revision: 1 };
    const pending: {
        objectId: string;
        identity: AudioIdentity;
        signal?: AbortSignal;
        resolve: (value: PreviewEnvelope) => void;
        reject: (error: Error) => void;
    }[] = [];
    const preview = vi.fn(
        (_sessionId: number, objectId: string, _bins: number, signal?: AbortSignal) =>
            new Promise<PreviewEnvelope>((resolve, reject) => {
                pending.push({ objectId, identity: { ...identity }, signal, resolve, reject });
            }),
    );
    const queue = new WaveformPreviewQueue({
        preview,
        audioIdentity: () => identity,
    } as unknown as ImageTransport);
    const loaded = vi.fn();
    const failed = vi.fn();
    const request = (objectId: string) => queue.request(1, objectId, 1024, loaded, failed);
    const reply = (index: number, overrides: Partial<PreviewEnvelope> = {}): PreviewEnvelope => ({
        ...pending[index]!.identity,
        objectId: pending[index]!.objectId,
        warnings: [],
        lanes: [
            {
                role: 'MONO',
                sourceObjectId: pending[index]!.objectId,
                sampleRate: 33333,
                sampleWidthBytes: 2,
                nativeBitDepth: 12,
                sampleRateApproximate: true,
                storedFrameCount: 5,
                playbackStartFrame: 0,
                playbackLengthFrames: 5,
                loopStartFrame: 2,
                loopLengthFrames: 3,
                bins: [{ minimum: -1, maximum: 0.5 }],
            },
        ],
        ...overrides,
    });
    return {
        queue,
        pending,
        preview,
        loaded,
        failed,
        request,
        reply,
        changeIdentity: (change: Partial<AudioIdentity>) => {
            identity = { ...identity, ...change };
        },
    };
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

describe('WaveformPreviewQueue', () => {
    it('runs at most two reads and releases capacity after failures as well as successes', async () => {
        const f = fixture();
        for (const id of ['one', 'two', 'three', 'four']) f.request(id);
        expect(f.preview).toHaveBeenCalledTimes(2);
        const error = new Error('read failed');
        f.pending[0]!.reject(error);
        await settle();
        expect(f.failed).toHaveBeenCalledExactlyOnceWith(error);
        expect(f.preview).toHaveBeenCalledTimes(3);
        expect(f.pending[2]!.objectId).toBe('three');
        f.pending[1]!.resolve(f.reply(1));
        await settle();
        expect(f.preview).toHaveBeenCalledTimes(4);
        expect(f.pending[3]!.objectId).toBe('four');
        f.pending[2]!.resolve(f.reply(2));
        f.pending[3]!.resolve(f.reply(3));
        await settle();
        expect(f.loaded).toHaveBeenCalledTimes(3);
    });

    it('aborts active reads, drops queued work and ignores late results after reset', async () => {
        const f = fixture();
        for (const id of ['one', 'two', 'queued-old']) f.request(id);
        expect(f.pending.map((request) => request.signal?.aborted)).toEqual([false, false]);
        f.queue.reset();
        expect(f.pending.map((request) => request.signal?.aborted)).toEqual([true, true]);
        f.request('new');
        f.pending[0]!.resolve(f.reply(0));
        f.pending[1]!.reject(new DOMException('Cancelled', 'AbortError'));
        await settle();
        expect(f.loaded).not.toHaveBeenCalled();
        expect(f.failed).not.toHaveBeenCalled();
        expect(f.preview).toHaveBeenCalledTimes(3);
        expect(f.pending[2]!.objectId).toBe('new');
        expect(f.pending[2]!.signal?.aborted).toBe(false);
        f.pending[2]!.resolve(f.reply(2));
        await settle();
        expect(f.loaded).toHaveBeenCalledExactlyOnceWith(f.reply(2));
    });

    it.each([{ revision: 2 }, { documentId: 'document-2' }, { imageId: 'image-2' }])(
        'discards active and queued requests after an identity change: %j',
        async (change) => {
            const f = fixture();
            for (const id of ['one', 'two', 'queued-old']) f.request(id);
            f.changeIdentity(change);
            f.pending[0]!.resolve(f.reply(0));
            f.pending[1]!.reject(new Error('old failure'));
            await settle();
            expect(f.preview).toHaveBeenCalledTimes(2);
            expect(f.loaded).not.toHaveBeenCalled();
            expect(f.failed).not.toHaveBeenCalled();
            f.request('current');
            f.pending[2]!.resolve(f.reply(2));
            await settle();
            expect(f.loaded).toHaveBeenCalledExactlyOnceWith(f.reply(2));
        },
    );

    it.each([{ revision: 2 }, { documentId: 'document-2' }, { objectId: 'other' }])(
        'reports a mismatched reply without publishing it: %j',
        async (mismatch) => {
            const f = fixture();
            f.request('one');
            f.pending[0]!.resolve(f.reply(0, mismatch));
            await settle();
            expect(f.loaded).not.toHaveBeenCalled();
            expect(f.failed).toHaveBeenCalledOnce();
            expect(f.failed.mock.calls[0]![0]).toBeInstanceOf(Error);
        },
    );
});
