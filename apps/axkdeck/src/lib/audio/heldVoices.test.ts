import { describe, expect, it, vi } from 'vitest';
import { HeldVoices, type PreparedVoice } from './heldVoices';

function setup() {
    const parameter = () => ({
        value: 1,
        setValueAtTime: vi.fn(),
        linearRampToValueAtTime: vi.fn(),
        cancelScheduledValues: vi.fn(),
    });
    const nodes: any[] = [];
    const context = {
        currentTime: 1,
        state: 'running',
        resume: vi.fn().mockResolvedValue(undefined),
        destination: {},
        createBufferSource: () => {
            const source = {
                context,
                playbackRate: parameter(),
                connect: vi.fn(() => gain),
                disconnect: vi.fn(),
                start: vi.fn(),
                stop: vi.fn(),
            };
            nodes.push(source);
            return source;
        },
        createGain: () => gain,
        createStereoPanner: () => pan,
    };
    const pan = { pan: parameter(), connect: vi.fn(() => context.destination), disconnect: vi.fn() };
    const gain = { gain: parameter(), connect: vi.fn(() => pan), disconnect: vi.fn() };
    const update = vi.fn(),
        held = new HeldVoices(
            () => context as unknown as AudioContext,
            () => held.stop(),
            update,
        );
    const voice: PreparedVoice = {
        buffer: { duration: 2 } as AudioBuffer,
        start: 0.1,
        length: 1,
        speed: 2,
        gain: 0.4,
        pan: -0.5,
        loop: true,
        loopStart: 0.2,
        loopEnd: 0.7,
    };
    return { held, context, nodes, update, voice };
}
describe('layered held voice output', () => {
    it('starts every matching voice together with individual pitch and loop settings', async () => {
        const { held, context, nodes, update, voice } = setup();
        await held.play(1, 'program', 'first', async () => [voice, { ...voice, speed: 1, loop: false }]);
        expect(context.resume).toHaveBeenCalledOnce();
        expect(nodes).toHaveLength(2);
        expect(nodes[0].start).toHaveBeenCalledWith(1.01, 0.1);
        expect(nodes[1].start).toHaveBeenCalledWith(1.01, 0.1, 1);
        expect(nodes[0].playbackRate.value).toBe(2);
        expect(nodes[1].playbackRate.value).toBe(1);
        expect(update).toHaveBeenLastCalledWith({
            objectId: 'program',
            status: 'playing',
            playheadFrame: 0,
            mapping: true,
        });
        held.stop('first');
        expect(nodes.every((node) => node.stop.mock.calls.length === 1)).toBe(true);
    });
    it('cancels preparation before release and does not allow stale releases to stop a replacement', async () => {
        const { held, nodes, voice } = setup();
        let complete!: (voices: PreparedVoice[]) => void;
        const first = held.play(1, 'program', 'first', async (_, signal) => {
            await new Promise<void>((resolve) => {
                complete = () => resolve();
            });
            signal.throwIfAborted();
            return [voice];
        });
        held.stop('first');
        complete([]);
        await first;
        expect(nodes).toHaveLength(0);
        await held.play(1, 'program', 'second', async () => [voice]);
        held.stop('first');
        expect(nodes[0].stop).not.toHaveBeenCalled();
        held.stop();
    });
    it('schedules the shared onset after all voice nodes have been allocated', async () => {
        const { held, context, nodes, voice } = setup();
        const allocate = context.createBufferSource;
        context.createBufferSource = () => {
            context.currentTime += 0.02;
            return allocate();
        };
        await held.play(1, 'program', 'slow-allocation', async () => [voice, voice]);
        const when = context.currentTime + 0.01;
        expect(nodes[0].start).toHaveBeenCalledWith(when, voice.start);
        expect(nodes[1].start).toHaveBeenCalledWith(when, voice.start);
        held.stop();
    });
    it('does not start any voice when preparation rejects', async () => {
        const { held, nodes, update } = setup();
        await held.play(1, 'program', 'first', async () => {
            throw new Error('Unsupported member');
        });
        expect(nodes).toHaveLength(0);
        expect(update).toHaveBeenLastCalledWith(
            expect.objectContaining({ status: 'failed', error: 'Unsupported member' }),
        );
    });
    it('validates the whole group before scheduling any output', async () => {
        const { held, nodes, update, voice } = setup();
        await held.play(1, 'program', 'invalid', async () => [voice, { ...voice, gain: NaN }]);
        expect(nodes.every((node) => !node.start.mock.calls.length)).toBe(true);
        expect(update).toHaveBeenLastCalledWith(expect.objectContaining({ status: 'failed' }));
    });
});
