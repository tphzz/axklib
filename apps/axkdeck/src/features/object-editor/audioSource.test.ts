import { describe, expect, it, vi } from 'vitest';
import { loadEditorAudio } from './audioSource';
import { fixtureFrames, fixtureRate, fixtureWave } from '../../test/sampleEditorAudio';
import type { ImageTransport } from '../../lib/transport';

const context = {
    createBuffer: (numberOfChannels: number, length: number, sampleRate: number) => {
        const data = new Float32Array(length);
        return { numberOfChannels, length, sampleRate, getChannelData: () => data };
    },
} as unknown as BaseAudioContext;
function setup() {
    const lane = {
        sampleRate: fixtureRate,
        frameCount: fixtureFrames,
        contentOffsetBytes: 0,
        wavSizeBytes: 44 + fixtureFrames * 2,
    };
    const bundle = { auditionId: 'fixture', contentSizeBytes: lane.wavSizeBytes, clips: [{ lanes: [lane] }] };
    const transport = {
        prepareAuditionBundle: vi.fn(async () => bundle),
        readAuditionContent: vi.fn(async () => fixtureWave()),
        deleteAudition: vi.fn(async () => undefined),
    };
    const controller = new AbortController();
    return {
        transport,
        bundle,
        lane,
        controller,
        load: () => loadEditorAudio(transport as unknown as ImageTransport, 1, 'sample', context, controller.signal),
    };
}
describe('editor source PCM', () => {
    it('keeps stereo lanes separate with source-rate frame coordinates', async () => {
        const { load, transport, bundle, lane } = setup();
        bundle.clips[0]!.lanes.push({ ...lane, contentOffsetBytes: lane.wavSizeBytes });
        bundle.contentSizeBytes *= 2;
        transport.readAuditionContent.mockImplementation(async () => {
            const bytes = new Uint8Array(bundle.contentSizeBytes);
            bytes.set(new Uint8Array(fixtureWave()));
            bytes.set(new Uint8Array(fixtureWave()), lane.wavSizeBytes);
            return bytes.buffer;
        });
        const source = await load();
        expect(source.lanes).toHaveLength(2);
        expect(source.lanes[0]).not.toBe(source.lanes[1]);
        expect(source.lanes[0]!.getChannelData(0)).toEqual(source.lanes[1]!.getChannelData(0));
        expect(source.frames).toBe(fixtureFrames);
    });
    it('decodes native-rate PCM and deletes the temporary audition', async () => {
        const { load, transport } = setup();
        const source = await load();
        expect(source.sampleRate).toBe(fixtureRate);
        expect(source.frames).toBe(fixtureFrames);
        expect(source.lanes[0]!.getChannelData(0)[1]).toBeCloseTo(
            Math.round(Math.sin(0.2) * Math.exp(-1 / 1900) * 0.8 * 32767) / 32768,
            6,
        );
        expect(transport.prepareAuditionBundle).toHaveBeenCalledWith(1, ['sample'], expect.any(AbortSignal), true);
        expect(transport.deleteAudition).toHaveBeenCalledWith('fixture');
    });
    it('rejects oversized audio before downloading and still cleans up', async () => {
        const { load, transport, bundle } = setup();
        bundle.contentSizeBytes = 128 * 1024 * 1024;
        await expect(load()).rejects.toThrow('working limit');
        expect(transport.readAuditionContent).not.toHaveBeenCalled();
        expect(transport.deleteAudition).toHaveBeenCalledOnce();
    });
    it('rejects descriptor mismatches instead of changing coordinates by resampling', async () => {
        const { load, transport, lane } = setup();
        lane.sampleRate = 48000;
        await expect(load()).rejects.toThrow('does not match');
        expect(transport.deleteAudition).toHaveBeenCalledOnce();
    });
    it('honors cancellation after download and releases the temporary resource', async () => {
        const { load, transport, controller } = setup();
        transport.readAuditionContent.mockImplementation(async () => {
            controller.abort();
            return fixtureWave();
        });
        await expect(load()).rejects.toMatchObject({ name: 'AbortError' });
        expect(transport.deleteAudition).toHaveBeenCalledOnce();
    });
});
