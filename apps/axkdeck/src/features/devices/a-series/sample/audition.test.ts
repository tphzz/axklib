import { afterEach, describe, expect, it, vi } from 'vitest';
import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';
import type { ImageTransport } from '../../../../lib/transport';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import { sampleValues } from './adapter';
import { draftPlaybackPlan, prepareSampleDraft } from './audition';

function audioBuffer(channels: number, length: number, sampleRate: number): AudioBuffer {
    const data = Array.from({ length: channels }, () => new Float32Array(length));
    return {
        numberOfChannels: channels,
        length,
        sampleRate,
        getChannelData: (channel: number) => data[channel]!,
        copyToChannel: (source: Float32Array, channel: number) => data[channel]!.set(source),
    } as AudioBuffer;
}

function auditionFixture() {
    const snapshot: SampleEditingSnapshot = {
        profile: 'a-series/sample',
        editable: true,
        reason: '',
        payloadSha256: 'a'.repeat(64),
        partitionIndex: 0,
        volumeName: 'Volume',
        parameters: { loop_mode: 4, loop_start_frame: 0, loop_length_frames: 0, root_key: 50, level: 127 },
        unavailableParameters: {},
        eqCoefficients: [],
        blockedParameters: ['loop_start_frame', 'loop_length_frames'],
        blockedParameterReasons: {
            loop_start_frame: 'The stereo channels store different values.',
            loop_length_frames: 'The stereo channels store different values.',
        },
        ...sampleFormatFixture('A3000_188'),
        playbackWindow: { start_frame: 0, length_frames: 15000 },
        canEditPlayback: true,
        maximumFrames: 15000,
        sources: ['left', 'right'].map((role) => ({ role, objectId: role, frames: 15000, sampleRate: 48000 })),
    };
    const source = {
        lanes: [audioBuffer(1, 15000, 48000), audioBuffer(1, 15000, 48000)],
        sampleRate: 48000,
        frames: 15000,
    };
    const context = { sampleRate: 48000, createBuffer: audioBuffer } as unknown as AudioContext;
    const transport = {} as ImageTransport;
    const renderedInputs: AudioBuffer[] = [];
    vi.stubGlobal(
        'OfflineAudioContext',
        class {
            destination = {};
            private source = {
                buffer: null as AudioBuffer | null,
                playbackRate: { value: 1 },
                connect: (next: unknown) => next,
                start: () => {},
            };
            constructor(
                private channels: number,
                private frames: number,
                private sampleRate: number,
            ) {}
            createBufferSource() {
                return this.source;
            }
            createGain() {
                return { gain: { value: 1 }, connect: (next: unknown) => next };
            }
            createStereoPanner() {
                return { pan: { value: 0 }, connect: (next: unknown) => next };
            }
            async startRendering() {
                renderedInputs.push(this.source.buffer!);
                return audioBuffer(this.channels, this.frames, this.sampleRate);
            }
        },
    );
    return {
        snapshot,
        source,
        renderedInputs,
        prepare: (changes: EditorValues = {}) =>
            prepareSampleDraft(
                transport,
                1,
                'rim',
                snapshot,
                { ...sampleValues(snapshot), ...changes },
                50,
                context,
                new AbortController().signal,
                { source },
            ),
    };
}

afterEach(() => vi.unstubAllGlobals());

describe('draft audition validation', () => {
    it('plays valid stereo PCM even when dormant loop fields are write-protected', async () => {
        const { prepare, source, renderedInputs } = auditionFixture();
        source.lanes[0]!.getChannelData(0)[10] = 0.25;
        source.lanes[1]!.getChannelData(0)[10] = -0.5;
        const prepared = await prepare();
        expect(prepared.descriptor).toMatchObject({ channels: 2, frameCount: 15000, loopMode: 4, loopLengthFrames: 0 });
        expect(renderedInputs[0]!.getChannelData(0)[10]).toBe(0.25);
        expect(renderedInputs[0]!.getChannelData(1)[10]).toBe(-0.5);
    });

    it('does not require a writable image to audition valid audio', async () => {
        const { prepare, snapshot } = auditionFixture();
        snapshot.editable = false;
        snapshot.reason = 'This image is read-only';
        snapshot.canEditPlayback = false;
        await expect(prepare()).resolves.toMatchObject({ descriptor: { frameCount: 15000 } });
    });

    it('ignores dormant loop offsets in nonrepeating playback', async () => {
        const { prepare, snapshot } = auditionFixture();
        snapshot.blockedParameters = [];
        await expect(prepare({ loop_start_frame: 900 })).resolves.toMatchObject({
            descriptor: { loopLengthFrames: 0 },
        });
    });

    it('still rejects repeating playback without a nonempty loop', async () => {
        const { prepare, snapshot } = auditionFixture();
        snapshot.blockedParameters = [];
        await expect(prepare({ loop_mode: 1 })).rejects.toThrow(/loop/i);
    });

    it.each([
        { loop_start_frame: 99, loop_length_frames: 300 },
        { loop_start_frame: 200, loop_length_frames: 301 },
    ])('rejects out-of-window source loops before pitch rounding: %j', async (loop) => {
        const { prepare, snapshot } = auditionFixture();
        snapshot.blockedParameters = [];
        await expect(
            prepare({
                'playback.start_frame': 100,
                'playback.length_frames': 400,
                coarse_tune: 24,
                loop_mode: 1,
                ...loop,
            }),
        ).rejects.toThrow(/loop/i);
    });

    it('still checks playback against every source lane', async () => {
        const { prepare, snapshot, source } = auditionFixture();
        snapshot.blockedParameters = [];
        source.lanes[1] = audioBuffer(1, 14999, 48000);
        await expect(prepare()).rejects.toThrow('stored Wave Data');
    });
});
describe('draft audition timing', () => {
    it('uses absolute source frames and transposes loop coordinates with pitch', () => {
        const plan = draftPlaybackPlan(
            {
                'playback.start_frame': 100,
                'playback.length_frames': 400,
                loop_start_frame: 200,
                loop_length_frames: 100,
                loop_mode: 1,
                root_key: 60,
                level: 127,
                pan: 63,
            },
            22050,
            44100,
            72,
        );
        expect(plan).toMatchObject({
            speed: 2,
            start: 100,
            length: 400,
            frames: 400,
            loopStart: 100,
            loopLength: 100,
            gain: 1,
            pan: 1,
            reverse: false,
        });
    });
    it('respects fixed pitch and reverse one-shot without inferring a repeating reverse loop', () => {
        const plan = draftPlaybackPlan(
            { 'playback.start_frame': 0, 'playback.length_frames': 100, loop_mode: 5, fixed_pitch: true, pan: -64 },
            44100,
            44100,
            72,
        );
        expect(plan.speed).toBe(1);
        expect(plan.reverse).toBe(true);
        expect(plan.loopMode).toBe(5);
        expect(plan.pan).toBe(0);
    });
});
