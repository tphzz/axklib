import { afterEach, describe, expect, it, vi } from 'vitest';

import { AudioImportAuditionController, type AudioImportAuditionState } from './audioImportAudition';

class MockAudioBuffer {
    readonly duration: number;
    readonly channels: Float32Array[];

    constructor(
        readonly length: number,
        readonly numberOfChannels = 1,
        readonly sampleRate = 48_000,
    ) {
        this.duration = length / sampleRate;
        this.channels = Array.from({ length: numberOfChannels }, () => new Float32Array(length));
    }

    getChannelData(channel: number): Float32Array {
        return this.channels[channel]!;
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
    static decodeError: Error | undefined;
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
    readonly decodeAudioData = vi.fn(async () => {
        if (MockAudioContext.decodeError) throw MockAudioContext.decodeError;
        return MockAudioContext.nextBuffer as unknown as AudioBuffer;
    });

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

    createBuffer(numberOfChannels: number, length: number, sampleRate: number): AudioBuffer {
        return new MockAudioBuffer(length, numberOfChannels, sampleRate) as unknown as AudioBuffer;
    }
}

const audioBlob = new Blob([new Uint8Array(32)], { type: 'audio/wav' });

function pcm16Wave(samples: readonly number[], sampleRate = 44_100): Blob {
    const dataBytes = samples.length * 2;
    const bytes = new Uint8Array(52 + dataBytes);
    const view = new DataView(bytes.buffer);
    const text = (offset: number, value: string): void => {
        for (let index = 0; index < value.length; ++index) bytes[offset + index] = value.charCodeAt(index);
    };
    text(0, 'RIFF');
    view.setUint32(4, bytes.length - 8, true);
    text(8, 'WAVE');
    text(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, sampleRate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    text(36, 'data');
    view.setUint32(40, dataBytes, true);
    samples.forEach((sample, index) => view.setInt16(44 + index * 2, sample, true));
    text(44 + dataBytes, 'JUNK');
    view.setUint32(48 + dataBytes, 0, true);
    return new Blob([bytes], { type: 'audio/wav' });
}

describe('AudioImportAuditionController', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        MockAudioContext.instances = [];
        MockAudioContext.nextBuffer = new MockAudioBuffer(480);
        MockAudioContext.decodeError = undefined;
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

    it('decodes valid PCM WAV without relying on the browser decoder', async () => {
        vi.stubGlobal('AudioContext', MockAudioContext);
        MockAudioContext.decodeError = new DOMException('Decoding failed');
        const states: AudioImportAuditionState[] = [];
        const controller = new AudioImportAuditionController((state) => states.push(state));

        await controller.play({
            rowId: 5,
            loadBlob: vi.fn().mockResolvedValue(pcm16Wave([-32_768, 0, 32_767])),
            timeline: {
                frameCount: 3,
                sampleRate: 44_100,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
            },
        });

        const source = MockAudioContext.instances[0]!.sources[0]!;
        const decoded = source.buffer as unknown as MockAudioBuffer;
        expect(decoded.sampleRate).toBe(44_100);
        expect(Array.from(decoded.getChannelData(0))).toEqual([-1, 0, 32_767 / 32_768]);
        expect(MockAudioContext.instances[0]!.decodeAudioData).not.toHaveBeenCalled();
        expect(states.at(-1)).toEqual({ rowId: 5, status: 'playing', error: '' });
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
