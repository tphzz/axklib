import { afterEach, describe, expect, it, vi } from 'vitest';

import type { AuditionDescriptor, ImageTransport } from '../transport';
import { AuditionController, type AuditionState } from './auditionController';

class MockAudioBuffer {
    readonly duration: number;
    private readonly channelData: Float32Array[];

    constructor(
        readonly length = 48_000,
        readonly numberOfChannels = 1,
        readonly sampleRate = 48_000,
        values?: number[],
    ) {
        this.duration = length / sampleRate;
        this.channelData = Array.from(
            { length: numberOfChannels },
            () => new Float32Array(values ?? Array.from({ length }, () => 0)),
        );
    }

    getChannelData(channel: number): Float32Array {
        return this.channelData[channel]!;
    }
}

class MockAudioParam {
    value = 1;
    cancelScheduledValues = vi.fn();
    setValueAtTime = vi.fn((value: number) => {
        this.value = value;
        return this;
    });
    linearRampToValueAtTime = vi.fn((value: number) => {
        this.value = value;
        return this;
    });
}

class MockGainNode {
    readonly gain = new MockAudioParam();
    connect = vi.fn(() => this);
    disconnect = vi.fn();
}

class MockAudioBufferSourceNode {
    static instances: MockAudioBufferSourceNode[] = [];
    buffer: AudioBuffer | null = null;
    loop = false;
    loopStart = 0;
    loopEnd = 0;
    readonly playbackRate = new MockAudioParam();
    onended: (() => void) | null = null;
    connect = vi.fn(() => this);
    disconnect = vi.fn();
    start = vi.fn();
    stop = vi.fn();

    constructor() {
        MockAudioBufferSourceNode.instances.push(this);
    }
}

class MockConstantSourceNode {
    static instances: MockConstantSourceNode[] = [];
    readonly offset = new MockAudioParam();
    connect = vi.fn(() => this);
    disconnect = vi.fn();
    start = vi.fn();
    stop = vi.fn();

    constructor() {
        MockConstantSourceNode.instances.push(this);
    }
}

class MockAudioContext {
    static instances: MockAudioContext[] = [];
    static nextBuffers: MockAudioBuffer[] = [];
    static initialState: AudioContextState = 'suspended';
    static resumeFailuresRemaining = 0;
    readonly destination = {};
    readonly sampleRate = 48_000;
    readonly baseLatency = 0.01;
    readonly outputLatency = 0.02;
    readonly sources: MockAudioBufferSourceNode[] = [];
    readonly gains: MockGainNode[] = [];
    currentTime = 1;
    onstatechange: (() => void) | null = null;
    state: AudioContextState;
    readonly decodeAudioData = vi.fn(async () => {
        events.push('decode');
        return (MockAudioContext.nextBuffers.shift() ?? new MockAudioBuffer()) as unknown as AudioBuffer;
    });

    constructor(options?: AudioContextOptions) {
        expect(options).toBeUndefined();
        this.state = MockAudioContext.initialState;
        MockAudioContext.instances.push(this);
        events.push('context');
    }

    readonly resume = vi.fn(async (): Promise<void> => {
        events.push('resume');
        if (MockAudioContext.resumeFailuresRemaining > 0) {
            MockAudioContext.resumeFailuresRemaining -= 1;
            throw new Error('Audio output is not available yet');
        }
        this.state = 'running';
    });

    readonly close = vi.fn(async (): Promise<void> => {
        events.push('close');
        this.state = 'closed';
    });

    createBufferSource(): AudioBufferSourceNode {
        const source = new MockAudioBufferSourceNode();
        this.sources.push(source);
        return source as unknown as AudioBufferSourceNode;
    }

    createGain(): GainNode {
        const gain = new MockGainNode();
        this.gains.push(gain);
        return gain as unknown as GainNode;
    }

    createConstantSource(): ConstantSourceNode {
        return new MockConstantSourceNode() as unknown as ConstantSourceNode;
    }

    getOutputTimestamp(): AudioTimestamp {
        return { contextTime: this.currentTime, performanceTime: performance.now() };
    }
}

class MockOfflineAudioContext {
    static instances: MockOfflineAudioContext[] = [];
    readonly sampleRate: number;
    readonly decodeAudioData = vi.fn(async () => {
        events.push('offline-decode');
        return (MockAudioContext.nextBuffers.shift() ?? new MockAudioBuffer()) as unknown as AudioBuffer;
    });

    constructor(
        readonly numberOfChannels: number,
        readonly length: number,
        sampleRate: number,
    ) {
        this.sampleRate = sampleRate;
        MockOfflineAudioContext.instances.push(this);
        events.push('offline-context');
    }
}

const events: string[] = [];

function descriptor(overrides: Partial<AuditionDescriptor> = {}): AuditionDescriptor {
    return {
        auditionId: 'audition-1',
        objectId: 'SMPL-1',
        sampleRate: 48_000,
        channels: 1,
        sampleWidthBytes: 2,
        frameCount: 48_000,
        wavSizeBytes: 96_044,
        loopMode: 0,
        loopModeLabel: 'off',
        loopStartFrame: 0,
        loopLengthFrames: 0,
        warnings: [],
        ...overrides,
    };
}

function transportFor(value: AuditionDescriptor): ImageTransport {
    return {
        prepareAudition: vi.fn(async () => {
            events.push('prepare');
            return value;
        }),
        readAuditionAudio: vi.fn(async () => {
            events.push('read');
            return new ArrayBuffer(value.wavSizeBytes);
        }),
        deleteAudition: vi.fn(async () => {
            events.push('delete');
        }),
    } as unknown as ImageTransport;
}

describe('AuditionController', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.useRealTimers();
    });

    function installAudio(): void {
        events.length = 0;
        MockAudioContext.instances = [];
        MockAudioContext.nextBuffers = [];
        MockAudioContext.initialState = 'suspended';
        MockAudioContext.resumeFailuresRemaining = 0;
        MockOfflineAudioContext.instances = [];
        MockAudioBufferSourceNode.instances = [];
        MockConstantSourceNode.instances = [];
        vi.stubGlobal('AudioContext', MockAudioContext);
        vi.stubGlobal('OfflineAudioContext', MockOfflineAudioContext);
        vi.stubGlobal(
            'requestAnimationFrame',
            vi.fn(() => 1),
        );
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
    }

    it('warms one output context and reuses it across stop, replay, and natural completion', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const updates: AuditionState[] = [];
        const controller = new AuditionController(transport, (state) => updates.push(state));

        await controller.warmup();
        await controller.play(1, 'SMPL-1');

        expect(events.indexOf('resume')).toBeLessThan(events.indexOf('prepare'));
        expect(events.indexOf('read')).toBeLessThan(events.indexOf('decode'));
        expect(MockAudioBufferSourceNode.instances[0]?.start).toHaveBeenCalledWith(1.01, 0);
        const gain = MockAudioContext.instances[0]?.gains[1]?.gain;
        expect(gain?.setValueAtTime).toHaveBeenNthCalledWith(1, 0, 1);
        expect(gain?.setValueAtTime).toHaveBeenNthCalledWith(2, 0, 1.01);
        expect(gain?.linearRampToValueAtTime).toHaveBeenCalledWith(1, 1.015);
        expect(updates.at(-1)).toEqual({ objectId: 'SMPL-1', status: 'playing', playheadFrame: 0 });

        await controller.stop();
        await controller.play(1, 'SMPL-1');

        expect(MockAudioContext.instances).toHaveLength(1);
        expect(MockAudioContext.instances[0]?.state).toBe('running');
        expect(transport.prepareAudition).toHaveBeenCalledTimes(1);
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(1);
        expect(MockAudioContext.instances[0]?.decodeAudioData).toHaveBeenCalledTimes(1);
        expect(MockAudioBufferSourceNode.instances).toHaveLength(2);

        MockAudioBufferSourceNode.instances[1]?.onended?.();
        expect(MockAudioContext.instances[0]?.state).toBe('running');
        expect(MockConstantSourceNode.instances[0]?.stop).not.toHaveBeenCalled();

        await controller.dispose();
        expect(MockConstantSourceNode.instances[0]?.stop).toHaveBeenCalledTimes(1);
        expect(MockAudioContext.instances[0]?.close).toHaveBeenCalledTimes(1);
    });

    it('does not resume an output context that is already running', async () => {
        installAudio();
        MockAudioContext.initialState = 'running';
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        await controller.warmup();
        await controller.play(1, 'SMPL-1');

        expect(MockAudioContext.instances[0]?.resume).not.toHaveBeenCalled();
        await controller.dispose();
    });

    it('retries a startup resume failure when playback is requested', async () => {
        installAudio();
        MockAudioContext.resumeFailuresRemaining = 1;
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        await expect(controller.warmup()).resolves.toBeUndefined();
        expect(MockAudioContext.instances[0]?.state).toBe('suspended');

        await controller.play(1, 'SMPL-1');

        expect(MockAudioContext.instances).toHaveLength(1);
        expect(MockAudioContext.instances[0]?.resume).toHaveBeenCalledTimes(2);
        expect(MockAudioContext.instances[0]?.state).toBe('running');
        await controller.dispose();
    });

    it('shares one resume attempt between startup warmup and immediate playback', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        const warming = controller.warmup();
        const playing = controller.play(1, 'SMPL-1');
        await Promise.all([warming, playing]);

        expect(MockAudioContext.instances).toHaveLength(1);
        expect(MockAudioContext.instances[0]?.resume).toHaveBeenCalledTimes(1);
        expect(MockConstantSourceNode.instances).toHaveLength(1);
        await controller.dispose();
    });

    it('recreates the persistent graph if the output context is externally closed', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        await controller.warmup();
        MockAudioContext.instances[0]!.state = 'closed';
        await controller.play(1, 'SMPL-1');

        expect(MockAudioContext.instances).toHaveLength(2);
        expect(MockConstantSourceNode.instances[0]?.stop).toHaveBeenCalledTimes(1);
        expect(MockConstantSourceNode.instances[1]?.start).toHaveBeenCalledTimes(1);
        await controller.dispose();
    });

    it('keeps the output context available after playback preparation fails', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        vi.mocked(transport.prepareAudition).mockRejectedValueOnce(new Error('Preparation failed'));
        const updates: AuditionState[] = [];
        const controller = new AuditionController(transport, (state) => updates.push(state));

        await controller.warmup();
        await controller.play(1, 'SMPL-1');

        expect(updates.at(-1)?.status).toBe('failed');
        expect(MockAudioContext.instances[0]?.state).toBe('running');
        expect(MockConstantSourceNode.instances[0]?.stop).not.toHaveBeenCalled();
        await controller.dispose();
    });

    it('uses native forward loop points and recreates the source when seeking', async () => {
        installAudio();
        const transport = transportFor(
            descriptor({
                frameCount: 100,
                sampleRate: 100,
                wavSizeBytes: 244,
                loopMode: 1,
                loopStartFrame: 10,
                loopLengthFrames: 20,
            }),
        );
        MockAudioContext.nextBuffers = [new MockAudioBuffer(48_000)];
        const controller = new AuditionController(transport, () => undefined);

        await controller.play(1, 'SMPL-1');
        controller.seek(15);

        expect(MockAudioBufferSourceNode.instances[0]).toMatchObject({ loop: true, loopStart: 0.1, loopEnd: 0.3 });
        expect(MockAudioBufferSourceNode.instances[1]?.start).toHaveBeenCalledWith(1.01, 0.15);
        await controller.dispose();
    });

    it('plays Sample Bank members once in order without repeating forward loops', async () => {
        installAudio();
        const transport = transportFor(
            descriptor({
                frameCount: 100,
                sampleRate: 100,
                wavSizeBytes: 244,
                loopMode: 1,
                loopStartFrame: 10,
                loopLengthFrames: 20,
            }),
        );
        const completed = vi.fn();
        const controller = new AuditionController(transport, () => undefined);

        controller.playSequence(1, ['SBNK-1', 'SBNK-2'], completed);
        await vi.waitFor(() => expect(MockAudioBufferSourceNode.instances).toHaveLength(1));
        expect(MockAudioBufferSourceNode.instances[0]?.loop).toBe(false);

        MockAudioBufferSourceNode.instances[0]?.onended?.();
        await vi.waitFor(() => expect(MockAudioBufferSourceNode.instances).toHaveLength(2));
        expect(MockAudioBufferSourceNode.instances[1]?.loop).toBe(false);

        MockAudioBufferSourceNode.instances[1]?.onended?.();
        expect(completed).toHaveBeenCalledWith({ playedCount: 2, skippedCount: 0 });
        expect(transport.prepareAudition).toHaveBeenNthCalledWith(1, 1, 'SBNK-1');
        expect(transport.prepareAudition).toHaveBeenNthCalledWith(2, 1, 'SBNK-2');
        expect(MockAudioContext.instances).toHaveLength(1);
        await controller.dispose();
    });

    it('skips unplayable Sample Bank members and continues the sequence', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        vi.mocked(transport.prepareAudition).mockRejectedValueOnce(new Error('Unsupported Sample'));
        const completed = vi.fn();
        const controller = new AuditionController(transport, () => undefined);

        controller.playSequence(1, ['SBNK-BROKEN', 'SBNK-OK'], completed);
        await vi.waitFor(() => expect(MockAudioBufferSourceNode.instances).toHaveLength(1));
        MockAudioBufferSourceNode.instances[0]?.onended?.();

        expect(completed).toHaveBeenCalledWith({ playedCount: 1, skippedCount: 1 });
        expect(transport.prepareAudition).toHaveBeenCalledTimes(2);
        await controller.dispose();
    });

    it('does not advance a Sample Bank sequence after explicit stop', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const completed = vi.fn();
        const controller = new AuditionController(transport, () => undefined);

        controller.playSequence(1, ['SBNK-1', 'SBNK-2'], completed);
        await vi.waitFor(() => expect(MockAudioBufferSourceNode.instances).toHaveLength(1));
        await controller.stop();
        MockAudioBufferSourceNode.instances[0]?.onended?.();
        await Promise.resolve();

        expect(transport.prepareAudition).toHaveBeenCalledTimes(1);
        expect(completed).not.toHaveBeenCalled();
        await controller.dispose();
    });

    it('reverses reverse-mode buffers once and plays them from the beginning', async () => {
        installAudio();
        const buffer = new MockAudioBuffer(3, 1, 3, [1, 2, 3]);
        MockAudioContext.nextBuffers = [buffer];
        const transport = transportFor(
            descriptor({ frameCount: 3, sampleRate: 3, wavSizeBytes: 50, loopMode: 3, loopModeLabel: 'reverse' }),
        );
        const controller = new AuditionController(transport, () => undefined);

        await controller.play(1, 'SMPL-1');

        expect(Array.from(buffer.getChannelData(0))).toEqual([3, 2, 1]);
        expect(MockAudioBufferSourceNode.instances[0]?.start).toHaveBeenCalledWith(1.01, 0);
        await controller.dispose();
    });

    it('skips oversized speculative audio but permits an explicit transient play', async () => {
        installAudio();
        const value = descriptor({ frameCount: 4, wavSizeBytes: 52 });
        const transport = transportFor(value);
        MockAudioContext.nextBuffers = [new MockAudioBuffer(4)];
        const controller = new AuditionController(transport, () => undefined, undefined, undefined, {
            cacheBudgetBytes: 8,
            maximumCacheEntries: 1,
        });

        await controller.prefetch(1, 'SMPL-1');
        expect(transport.readAuditionAudio).not.toHaveBeenCalled();

        await controller.play(1, 'SMPL-1');
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(1);
        await controller.stop();
        await controller.play(1, 'SMPL-1');
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(2);
        await controller.dispose();
    });

    it('rejects explicit playback above the encoded and decoded working-set budget before fetching', async () => {
        installAudio();
        const transport = transportFor(descriptor({ frameCount: 4, wavSizeBytes: 52 }));
        const states: AuditionState[] = [];
        const controller = new AuditionController(transport, (state) => states.push(state), undefined, undefined, {
            maximumWorkingSetBytes: 60,
        });

        await controller.play(1, 'SMPL-1');

        expect(transport.readAuditionAudio).not.toHaveBeenCalled();
        expect(states.at(-1)).toMatchObject({
            objectId: 'SMPL-1',
            status: 'failed',
            error: 'Audio is too large to audition safely',
        });
        await controller.dispose();
    });

    it('reuses selected-object prefetches and invalidates them when the image session closes', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        await controller.prefetch(7, 'SMPL-1');
        await controller.play(7, 'SMPL-1');
        await controller.stop();

        expect(transport.prepareAudition).toHaveBeenCalledTimes(1);
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(1);

        await controller.invalidateSession(7);
        await controller.play(7, 'SMPL-1');

        expect(transport.prepareAudition).toHaveBeenCalledTimes(2);
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(2);
        expect(MockAudioContext.instances).toHaveLength(1);
        expect(MockAudioContext.instances[0]?.state).toBe('running');
        await controller.dispose();
    });

    it('does not create the output AudioContext during speculative prefetch', async () => {
        installAudio();
        const transport = transportFor(descriptor());
        const controller = new AuditionController(transport, () => undefined);

        await controller.prefetch(7, 'SMPL-1');

        expect(MockAudioContext.instances).toHaveLength(0);
        expect(MockOfflineAudioContext.instances).toHaveLength(1);

        await controller.play(7, 'SMPL-1');

        expect(MockAudioContext.instances).toHaveLength(1);
        expect(transport.prepareAudition).toHaveBeenCalledTimes(1);
        expect(transport.readAuditionAudio).toHaveBeenCalledTimes(1);
        await controller.dispose();
    });
});
