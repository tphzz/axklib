import { describe, expect, it, vi } from 'vitest';
import type { AuditionBundleDescriptor, ImageTransport } from '../transport';
import { AuditionAssetStore } from './auditionAssetStore';

type Identity = { imageId: string; documentId: string; revision: number };
const initialIdentity: Identity = { imageId: 'image-1', documentId: 'document-1', revision: 1 };
function bundle(identity: Identity, auditionId = 'asset-1'): AuditionBundleDescriptor {
    return {
        ...identity,
        auditionId,
        contentSizeBytes: 52,
        clips: [
            {
                objectId: 'wave-1',
                direction: 'FORWARD',
                looping: false,
                warnings: [],
                lanes: [
                    {
                        role: 'MONO',
                        sourceObjectId: 'wave-1',
                        sampleRate: 4,
                        sampleWidthBytes: 2,
                        frameCount: 4,
                        contentOffsetBytes: 0,
                        wavSizeBytes: 52,
                        loopStartFrame: 0,
                        loopLengthFrames: 0,
                    },
                ],
            },
        ],
    };
}
function decoded(value = 1): AudioBuffer {
    const data = new Float32Array(4).fill(value);
    return { length: 4, sampleRate: 4, numberOfChannels: 1, getChannelData: () => data } as unknown as AudioBuffer;
}
function deferred<T>() {
    let resolve!: (value: T) => void;
    const promise = new Promise<T>((done) => {
        resolve = done;
    });
    return { promise, resolve };
}
function setup() {
    let identity = { ...initialIdentity };
    let nextId = 0;
    const transport = {
        audioIdentity: vi.fn(() => ({ ...identity })),
        prepareAuditionBundle: vi.fn<ImageTransport['prepareAuditionBundle']>(async () =>
            bundle(identity, `asset-${++nextId}`),
        ),
        readAuditionContent: vi.fn(async () => new ArrayBuffer(52)),
        deleteAudition: vi.fn(async () => {}),
    };
    const context = { sampleRate: 4, decodeAudioData: vi.fn(async () => decoded()) };
    const store = new AuditionAssetStore(transport as unknown as ImageTransport, () => {}, {});
    return {
        transport,
        context,
        store,
        change: (next: Partial<Identity>) => {
            identity = { ...identity, ...next };
        },
        load: () => store.load(1, 'wave-1', context as unknown as BaseAudioContext, false),
    };
}

describe('audition asset identity', () => {
    it('replaces pending speculative preparation with foreground playback and cleans both resources', async () => {
        const fixture = setup();
        const preparation = deferred<AuditionBundleDescriptor>();
        fixture.transport.prepareAuditionBundle.mockImplementationOnce(() => preparation.promise);
        const speculative = fixture.store.load(1, 'wave-1', undefined, true).catch((error: unknown) => error);
        const foreground = fixture.load();
        try {
            expect(fixture.transport.prepareAuditionBundle).toHaveBeenCalledTimes(2);
            expect(fixture.transport.prepareAuditionBundle.mock.calls[1]?.[2]).toMatchObject({
                readIntent: 'PLAYBACK',
            });
            expect(await foreground).not.toBeNull();
        } finally {
            preparation.resolve(bundle(initialIdentity, 'cancelled-prefetch'));
            await Promise.allSettled([speculative, foreground]);
            await fixture.store.dispose();
        }
        expect(await speculative).toMatchObject({ name: 'AbortError' });
        expect(fixture.transport.deleteAudition).toHaveBeenCalledWith('cancelled-prefetch');
    });

    it.each([{ imageId: 'image-2' }, { documentId: 'document-2' }, { revision: 2 }])(
        'separates cached PCM when identity changes by %j',
        async (change) => {
            const fixture = setup();
            try {
                const first = await fixture.load();
                expect(await fixture.load()).toBe(first);
                expect(fixture.transport.prepareAuditionBundle).toHaveBeenCalledTimes(1);
                fixture.change(change);
                fixture.context.decodeAudioData.mockResolvedValueOnce(decoded(2));
                const second = await fixture.load();
                expect(fixture.transport.prepareAuditionBundle).toHaveBeenCalledTimes(2);
                expect(second).not.toBe(first);
                expect(second?.buffer.getChannelData(0)[0]).toBe(2);
                expect(await fixture.load()).toBe(second);
            } finally {
                await fixture.store.dispose();
            }
        },
    );

    it.each(
        (['prepare', 'read', 'decode'] as const).flatMap((stage) => [
            { stage, change: { documentId: 'document-2' } },
            { stage, change: { revision: 2 } },
        ]),
    )('discards and cleans a late $stage response after $change', async ({ stage, change }) => {
        const fixture = setup();
        const preparation = deferred<AuditionBundleDescriptor>();
        const content = deferred<ArrayBuffer>();
        const decoding = deferred<AudioBuffer>();
        if (stage === 'prepare')
            fixture.transport.prepareAuditionBundle.mockImplementationOnce(() => preparation.promise);
        if (stage === 'read') fixture.transport.readAuditionContent.mockImplementationOnce(() => content.promise);
        if (stage === 'decode') fixture.context.decodeAudioData.mockImplementationOnce(() => decoding.promise);
        const pending = fixture.load();
        const outcome = pending.then(
            (value) => ({ value, error: null }),
            (error: unknown) => ({ value: null, error }),
        );
        if (stage === 'read')
            await vi.waitFor(() => expect(fixture.transport.readAuditionContent).toHaveBeenCalledOnce());
        if (stage === 'decode') await vi.waitFor(() => expect(fixture.context.decodeAudioData).toHaveBeenCalledOnce());
        fixture.change(change);
        preparation.resolve(bundle(initialIdentity, 'late-asset'));
        content.resolve(new ArrayBuffer(52));
        decoding.resolve(decoded(9));
        try {
            expect(await outcome).toMatchObject({ value: null, error: { name: 'AbortError' } });
            expect(fixture.transport.deleteAudition).toHaveBeenCalledExactlyOnceWith(
                stage === 'prepare' ? 'late-asset' : 'asset-1',
            );
            if (stage === 'prepare') expect(fixture.transport.readAuditionContent).not.toHaveBeenCalled();
            if (stage !== 'decode') expect(fixture.context.decodeAudioData).not.toHaveBeenCalled();
            const current = await fixture.load();
            expect(fixture.transport.prepareAuditionBundle).toHaveBeenCalledTimes(2);
            expect(current?.buffer.getChannelData(0)[0]).toBe(1);
        } finally {
            await fixture.store.dispose();
        }
    });

    it('does not return stale PCM when revision changes during remote asset cleanup', async () => {
        const fixture = setup();
        const cleanup = deferred<void>();
        fixture.transport.deleteAudition.mockImplementationOnce(() => cleanup.promise);
        const pending = fixture.load();
        const outcome = pending.then(
            (value) => ({ value, error: null }),
            (error: unknown) => ({ value: null, error }),
        );
        await vi.waitFor(() => expect(fixture.transport.deleteAudition).toHaveBeenCalledOnce());
        fixture.change({ revision: 2 });
        cleanup.resolve();
        try {
            expect(await outcome).toMatchObject({ value: null, error: { name: 'AbortError' } });
            expect(fixture.transport.deleteAudition).toHaveBeenCalledTimes(1);
            await fixture.load();
            expect(fixture.transport.prepareAuditionBundle).toHaveBeenCalledTimes(2);
        } finally {
            await fixture.store.dispose();
        }
    });

    it('rejects a mismatched bundle identity before fetching bytes and still cleans it', async () => {
        const fixture = setup();
        fixture.transport.prepareAuditionBundle.mockResolvedValueOnce(bundle({ ...initialIdentity, revision: 2 }));
        try {
            await expect(fixture.load()).rejects.toMatchObject({ name: 'AbortError' });
            expect(fixture.transport.readAuditionContent).not.toHaveBeenCalled();
            expect(fixture.transport.deleteAudition).toHaveBeenCalledExactlyOnceWith('asset-1');
        } finally {
            await fixture.store.dispose();
        }
    });
});
