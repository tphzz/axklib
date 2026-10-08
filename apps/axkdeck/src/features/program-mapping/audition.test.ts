import { describe, expect, it, vi } from 'vitest';
import { MappingAudition, boundedMap } from './audition';
import { ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import { mappingDetail } from '../../test/mappingEditorFixture';
import { programEditorFixture } from '../../test/programEditorFixture';
import { fixtureFrames, fixtureRate, fixtureWave } from '../../test/sampleEditorAudio';
import type { ImageTransport } from '../../lib/transport';
import type { PreparedVoice } from '../../lib/audio/heldVoices';
import { MappingPcmCache } from './pcmCache';

function setup() {
    const fixture = programEditorFixture();
    const details = new Map(['sample', 'a', 'b', 'bank'].map((id) => [id, mappingDetail(id)]));
    details.set('program', fixture.document.detail!);
    const lane = {
        sampleRate: fixtureRate,
        frameCount: fixtureFrames,
        contentOffsetBytes: 0,
        wavSizeBytes: 44 + fixtureFrames * 2,
    };
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => details.get(id)!),
        programEditorCatalog: vi.fn(async () => fixture.catalog),
        prepareAuditionBundle: vi.fn(async () => ({
            auditionId: 'test',
            contentSizeBytes: lane.wavSizeBytes,
            clips: [{ lanes: [lane] }],
        })),
        readAuditionContent: vi.fn(async () => fixtureWave()),
        deleteAudition: vi.fn(async () => {}),
        startObjectParameterEdit: vi.fn(),
        waitForJob: vi.fn(),
    };
    const editors = new ObjectEditorWorkflow({ transport, refresh: vi.fn(), stopPlayback: vi.fn(), status: vi.fn() });
    const context = {
        sampleRate: fixtureRate,
        createBuffer: (numberOfChannels: number, length: number, sampleRate: number) => {
            const data = Array.from({ length: numberOfChannels }, () => new Float32Array(length));
            return {
                numberOfChannels,
                length,
                sampleRate,
                duration: length / sampleRate,
                getChannelData: (channel: number) => data[channel]!,
                copyToChannel: (source: Float32Array, channel: number) => data[channel]!.set(source),
            };
        },
    } as unknown as AudioContext;
    let prepared: PreparedVoice[] = [];
    const audio = {
        state: { objectId: null, status: 'idle' as const, playheadFrame: 0 },
        releaseVoices: vi.fn(),
        playVoices: vi.fn(
            async (
                _session: number,
                _id: string,
                _token: string,
                prepare: (context: AudioContext, signal: AbortSignal) => Promise<PreparedVoice[]>,
            ) => {
                prepared = await prepare(context, new AbortController().signal);
            },
        ),
    };
    const mapping = new MappingAudition(editors, transport as unknown as ImageTransport, audio);
    return { details, editors, transport, audio, mapping, prepared: () => prepared };
}
describe('all matching mapping audition', () => {
    it('shares Wave Data across different Samples without dropping their separate voices', async () => {
        const { editors, mapping, details, prepared, transport } = setup();
        for (const id of ['a', 'b']) {
            const detail = details.get(id)!;
            if (detail.editing?.profile === 'a-series/sample')
                detail.editing.sources = [
                    { objectId: 'shared-wave', role: 'MONO', frames: fixtureFrames, sampleRate: fixtureRate },
                ];
        }
        const bank = (await editors.load(1, 'bank'))!;
        await mapping.play(bank, 'members', 'shared', 60, 100);
        expect(prepared()).toHaveLength(2);
        expect(prepared()[0]!.buffer).toBe(prepared()[1]!.buffer);
        expect(transport.prepareAuditionBundle).toHaveBeenCalledTimes(1);
    });
    it('keeps distinct assignment voices while sharing source PCM and leaving selection/root untouched', async () => {
        const { editors, mapping, transport, prepared } = setup();
        const document = (await editors.load(1, 'program'))!;
        document.programAssignmentId = 1;
        await mapping.play(document, 'program', 'first', 60, 100);
        expect(prepared()).toHaveLength(2);
        expect(prepared()[0]!.buffer).toBe(prepared()[1]!.buffer);
        expect(transport.prepareAuditionBundle).toHaveBeenCalledTimes(1);
        expect(document.programAssignmentId).toBe(1);
        expect(document.draft.dirty).toBe(false);
        await mapping.play(document, 'program', 'second', 61, 100);
        expect(transport.prepareAuditionBundle).toHaveBeenCalledTimes(1);
    });
    it('uses canonical Sample/Bank drafts and inclusive velocity intersections across every bank member', async () => {
        const { editors, mapping, prepared, transport } = setup();
        const bank = (await editors.load(1, 'bank'))!,
            a = (await editors.load(1, 'a'))!,
            b = (await editors.load(1, 'b'))!;
        a.draft.patch({ velocity_high: 63 });
        b.draft.patch({ velocity_low: 64 });
        await mapping.play(bank, 'members', 'soft', 60, 63);
        expect(prepared()).toHaveLength(1);
        await mapping.play(bank, 'bank', 'hard', 60, 64);
        expect(prepared()).toHaveLength(1);
        expect(transport.prepareAuditionBundle).toHaveBeenCalledTimes(2);
        bank.draft.patch({ velocity_high: 70 });
        await mapping.play(bank, 'bank', 'excluded', 60, 100);
        expect(prepared()).toHaveLength(0);
        expect(bank.previewMemberId).toBeUndefined();
        expect(a.draft.values.root_key).toBe(60);
    });
    it('refuses unresolved assignments and incompatible stereo playback instead of silently omitting voices', async () => {
        const { editors, mapping, details, transport } = setup();
        const program = (await editors.load(1, 'program'))!;
        if (program.detail!.editing?.profile === 'a-series/program')
            program.detail!.editing.targets[0]!.available = false;
        await expect(mapping.play(program, 'program', 'missing', 60, 100)).rejects.toThrow('unresolved assignments');
        expect(transport.prepareAuditionBundle).not.toHaveBeenCalled();
        const detail = details.get('a')!;
        if (detail.editing?.profile === 'a-series/sample') detail.editing.blockedParameters = ['root_key'];
        const sample = (await editors.load(1, 'a'))!;
        await expect(mapping.play(sample, 'sample', 'stereo', 60, 100)).rejects.toThrow('stereo channel');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
    it('bounds aggregate working memory and source preparation concurrency', async () => {
        const cache = new MappingPcmCache({} as ImageTransport);
        const release = cache.reserve(100 * 1024 * 1024);
        expect(() => cache.reserve(30 * 1024 * 1024)).toThrow('no partial group');
        release();
        let active = 0,
            peak = 0;
        await boundedMap(
            Array.from({ length: 12 }, (_, index) => index),
            async (index) => {
                peak = Math.max(peak, ++active);
                await new Promise((resolve) => setTimeout(resolve, 1));
                active--;
                return index;
            },
        );
        expect(peak).toBe(4);
        expect(active).toBe(0);
    });
    it('stops scheduling remaining work after one source fails', async () => {
        let started = 0;
        await expect(
            boundedMap(
                Array.from({ length: 20 }, (_, index) => index),
                async (index) => {
                    started++;
                    if (index === 0) throw new Error('Missing source');
                    await new Promise((resolve) => setTimeout(resolve, 1));
                    return index;
                },
            ),
        ).rejects.toThrow('Missing source');
        await new Promise((resolve) => setTimeout(resolve, 5));
        expect(started).toBe(4);
    });
});
