import { afterEach, describe, expect, it, vi } from 'vitest';
import { MappingClient } from './client.svelte';
import { MappingController } from './controller.svelte';
import { programEditorFixture } from '../../test/programEditorFixture';
import type { MappingCommand } from './protocol';

describe('mapping selection feedback', () => {
    afterEach(() => vi.useRealTimers());
    it('retains an immediately completed gesture until its own selection acknowledgment arrives', async () => {
        const controller = new MappingController({ locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() });
        const { document } = programEditorFixture();
        controller.refresh(document);
        const commands: MappingCommand[] = [];
        const client = new MappingClient('program', {
            listen: vi.fn(),
            send: async (command) => {
                commands.push(command);
            },
        });
        client.receive({ state: controller.state });
        client.select(1);
        client.begin();
        client.change({ low: 20, high: 127, velocityLow: 0, velocityHigh: 127 }, ['low']);
        client.end();
        expect(commands).toHaveLength(1);
        client.receive(await controller.receive(commands[0]!));
        expect(commands).toHaveLength(2);
        expect(commands[1]!.action).toMatchObject({ kind: 'range', selectionId: 1 });
        client.receive(await controller.receive(commands[1]!));
        expect(document.draft.values['assignments.1.key_low']).toBe(20);
    });
    it('updates immediately, coalesces latest selection, and does not advance edit content', async () => {
        const controller = new MappingController({ locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() });
        controller.refresh(programEditorFixture().document);
        const commands: MappingCommand[] = [];
        const client = new MappingClient('program', {
            listen: vi.fn(),
            send: async (command) => {
                commands.push(command);
            },
        });
        client.receive({ state: controller.state });
        client.select(1);
        client.select(0);
        client.select(1);
        expect(client.state?.selectionId).toBe(1);
        expect(client.locked).toBe(false);
        expect(commands).toHaveLength(1);
        client.receive(await controller.receive(commands[0]!));
        expect(client.pending).toBe('');
        expect(client.state?.selectionId).toBe(1);
        expect(commands).toHaveLength(1);
    });
    it('cancels a local gesture on external content changes', () => {
        const controller = new MappingController({ locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() });
        const { document } = programEditorFixture();
        controller.refresh(document);
        const send = vi.fn().mockResolvedValue(undefined);
        const client = new MappingClient('program', { listen: vi.fn(), send });
        client.receive({ state: controller.state });
        client.begin();
        client.change({ low: 20, high: 127, velocityLow: 0, velocityHigh: 127 }, ['low']);
        document.draft.set('level', 34);
        controller.refresh(document);
        client.receive({ state: controller.state });
        client.end();
        expect(send).not.toHaveBeenCalled();
        expect(client.preview).toBeNull();
    });
    it.each(['note', 'release', 'lease'] as const)(
        'does not release a replacement when an earlier %s send fails',
        async (kind) => {
            vi.useFakeTimers();
            const controller = new MappingController({
                locked: false,
                save: vi.fn(),
                discard: vi.fn(),
                recover: vi.fn(),
            });
            controller.refresh(programEditorFixture().document);
            const commands: MappingCommand[] = [];
            let fail!: (error: Error) => void;
            const client = new MappingClient('program', {
                listen: vi.fn(),
                send: async (command) => {
                    commands.push(command);
                    if (command.action.kind === kind && command.action.sequence === 1)
                        await new Promise<void>((_, reject) => (fail = reject));
                },
            });
            client.receive({ state: controller.state });
            client.press(60, 100);
            if (kind === 'lease') await vi.advanceTimersByTimeAsync(2000);
            client.press(61, 100);
            fail(new Error('Earlier relay failed'));
            await Promise.resolve();
            await Promise.resolve();
            try {
                expect(
                    commands.some((command) => command.action.kind === 'release' && command.action.sequence === 2),
                ).toBe(false);
                expect(client.status).toBe('');
            } finally {
                client.release();
            }
        },
    );
    it('clears a prior audition error when retrying a valid key', () => {
        vi.useFakeTimers();
        const controller = new MappingController({ locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() });
        controller.refresh(programEditorFixture().document);
        const client = new MappingClient('program', { listen: vi.fn(), send: vi.fn().mockResolvedValue(undefined) });
        client.receive({ state: controller.state });
        client.status = 'A matching Sample is unavailable';
        client.press(60, 100);
        try {
            expect(client.status).toBe('');
        } finally {
            client.release();
        }
    });
});
