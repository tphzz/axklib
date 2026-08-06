import { afterEach, describe, expect, it, vi } from 'vitest';

import { AudioImportAuditionController, type AudioImportAuditionState } from './audioImportAudition';

class MockAudioBuffer {
    readonly duration: number;

    constructor(
        readonly length: number,
        readonly numberOfChannels = 1,
        readonly sampleRate = 48_000,
    ) {
        this.duration = length / sampleRate;
    }
}

class MockAudioParam {
    value = 1;
    setValueAtTime = vi.fn();
    linearRampToValueAtTime = vi.fn();
}

class MockGainNode {
    readonly gain = new MockAudioParam();
    connect = vi.fn();
    disconnect = vi.fn();
}

class MockAudioBufferSourceNode {
    buffer: AudioBuffer | null = null;
    loop = false;
    loopStart = 0;
    loopEnd = 0;
    onended: (() => void) | null = null;
    connect = vi.fn();
    disconnect = vi.fn();
    start = vi.fn();
    stop = vi.fn();
}

class MockAudioContext {
    static instances: MockAudioContext[] = [];
    static nextBuffer = new MockAudioBuffer(480);
    readonly destination = {};
    readonly currentTime = 1;
    readonly sources: MockAudioBufferSourceNode[] = [];
    state: AudioContextState = 'suspended';
    readonly resume = vi.fn(async () => {
        this.state = 'running';
    });
    readonly close = vi.fn(async () => {
        this.state = 'closed';
    });
    readonly decodeAudioData = vi.fn(async () => MockAudioContext.nextBuffer as unknown as AudioBuffer);

    constructor() {
        MockAudioContext.instances.push(this);
    }

    createBufferSource(): AudioBufferSourceNode {
        const source = new MockAudioBufferSourceNode();
        this.sources.push(source);
        return source as unknown as AudioBufferSourceNode;
    }

    createGain(): GainNode {
        return new MockGainNode() as unknown as GainNode;
    }
}

const audioBlob = new Blob([new Uint8Array(32)], { type: 'audio/wav' });

describe('AudioImportAuditionController', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        MockAudioContext.instances = [];
        MockAudioContext.nextBuffer = new MockAudioBuffer(480);
    });

    it('extends a short one-shot source to the shared 500 ms preview duration', async () => {
        vi.stubGlobal('AudioContext', MockAudioContext);
        const states: AudioImportAuditionState[] = [];
        const controller = new AudioImportAuditionController((state) => states.push(state));

        await controller.play({
            rowId: 7,
            loadBlob: vi.fn().mockResolvedValue(audioBlob),
            timeline: {
                frameCount: 441,
                sampleRate: 44_100,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
            },
        });

        const source = MockAudioContext.instances[0]!.sources[0]!;
        expect(source.loop).toBe(true);
        expect(source.loopStart).toBe(0);
        expect(source.loopEnd).toBe(0.01);
        expect(source.start).toHaveBeenCalledWith(1.01);
        expect(source.stop).toHaveBeenCalledWith(1.51);
        expect(states.at(-1)).toEqual({ rowId: 7, status: 'playing', error: '' });

        source.onended?.();
        expect(states.at(-1)).toEqual({ rowId: null, status: 'idle', error: '' });
        await controller.dispose();
    });

    it('uses current forward-loop fields and lets stop cancel indefinite playback', async () => {
        vi.stubGlobal('AudioContext', MockAudioContext);
        MockAudioContext.nextBuffer = new MockAudioBuffer(48_000);
        const states: AudioImportAuditionState[] = [];
        const loadBlob = vi.fn().mockResolvedValue(audioBlob);
        const controller = new AudioImportAuditionController((state) => states.push(state));

        await controller.play({
            rowId: 3,
            loadBlob,
            timeline: {
                frameCount: 44_100,
                sampleRate: 44_100,
                loopMode: 1,
                loopStartFrame: 4_410,
                loopLengthFrames: 22_050,
            },
        });

        const source = MockAudioContext.instances[0]!.sources[0]!;
        expect(source.loop).toBe(true);
        expect(source.loopStart).toBe(0.1);
        expect(source.loopEnd).toBe(0.6);
        expect(source.stop).not.toHaveBeenCalled();

        controller.stop(3);
        expect(source.stop).toHaveBeenCalledOnce();
        expect(states.at(-1)).toEqual({ rowId: null, status: 'idle', error: '' });
        await controller.dispose();
    });

    it('does not start a source after preparation is stopped', async () => {
        vi.stubGlobal('AudioContext', MockAudioContext);
        let finishLoad!: (blob: Blob) => void;
        const loadBlob = vi.fn(
            () =>
                new Promise<Blob>((resolve) => {
                    finishLoad = resolve;
                }),
        );
        const states: AudioImportAuditionState[] = [];
        const controller = new AudioImportAuditionController((state) => states.push(state));

        const playing = controller.play({
            rowId: 9,
            loadBlob,
            timeline: {
                frameCount: 441,
                sampleRate: 44_100,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
            },
        });
        await vi.waitFor(() => expect(loadBlob).toHaveBeenCalledOnce());
        controller.stop(9);
        finishLoad(audioBlob);
        await playing;

        expect(MockAudioContext.instances[0]!.sources).toHaveLength(0);
        expect(states.at(-1)).toEqual({ rowId: null, status: 'idle', error: '' });
        await controller.dispose();
    });
});
