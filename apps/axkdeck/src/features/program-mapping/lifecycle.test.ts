import { act, fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { programEditorFixture } from '../../test/programEditorFixture';
import MappingWindow from './MappingWindow.svelte';
import { MappingController } from './controller.svelte';
import type { MappingCommand, MappingMessage } from './protocol';

function owner(name: string) {
    const workflow = { locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() };
    const controller = new MappingController(workflow);
    const { document, editing } = programEditorFixture();
    editing.programName = name;
    controller.refresh(document);
    return { controller, document };
}

async function windowFixture(controller: MappingController) {
    let publish: (message: MappingMessage) => void = () => {};
    const commands: MappingCommand[] = [];
    const stop = vi.fn();
    const view = render(MappingWindow, {
        adapter: {
            listen: async (callback) => {
                publish = callback;
                return stop;
            },
            send: async (command) => {
                commands.push(command);
            },
        },
    });
    await waitFor(() => expect(commands).toHaveLength(1));
    await act(() => publish({ state: controller.state, requestId: commands[0]!.requestId }));
    return { view, commands, stop, publish };
}

afterEach(() => vi.useRealTimers());

describe('Mapping Editor owner lifecycle', () => {
    it('ignores equal-version snapshot bodies while still completing their acknowledgement', async () => {
        const { controller, document } = owner('Original');
        document.draft.set('level', 40);
        controller.refresh(document);
        const { view, publish, commands } = await windowFixture(controller);
        await fireEvent.click(view.getByRole('button', { name: /^Save$/ }));
        const requestId = commands.at(-1)!.requestId;
        await act(() =>
            publish({ state: { ...controller.state, title: 'Repeated body' }, requestId, error: 'Save was rejected' }),
        );
        expect(view.getByRole('heading', { name: 'Mapping Editor: Original' })).toBeTruthy();
        expect(view.queryByRole('heading', { name: 'Mapping Editor: Repeated body' })).toBeNull();
        expect(view.getByRole('button', { name: /^Save$/ })).toHaveProperty('disabled', false);
        expect(view.getByText('Save was rejected')).toBeTruthy();
    });
    it('updates conflict guards without any draft, phase or status change', async () => {
        const { controller, document } = owner('Original');
        const original = controller.state;
        document.conflict = 'The Program changed externally';
        controller.refresh(document);
        expect(controller.state.editable).toBe(false);
        expect(controller.state.canDiscard).toBe(true);
        expect(controller.state.version).toBeGreaterThan(original.version);

        document.conflict = '';
        controller.refresh(document);
        expect(controller.state.editable).toBe(true);
        expect(controller.state.canDiscard).toBe(false);
    });

    it('revokes mapping edits when admission becomes read-only beneath an existing validation error', () => {
        const { controller, document } = owner('Original');
        document.inputErrors = { level: 'Enter an integer' };
        controller.refresh(document);
        const original = controller.state;
        expect(original.editable).toBe(true);
        const detail = document.detail!;
        if (detail.editing?.profile !== 'a-series/program') throw new Error('Program fixture required');
        document.detail = {
            ...detail,
            editing: { ...detail.editing, editable: false, reason: 'The image is now read-only' },
        };
        controller.refresh(document);
        expect(controller.state.editable).toBe(false);
        expect(controller.state.version).toBeGreaterThan(original.version);
    });

    it('reattaches a surviving window to a replacement controller through ready', async () => {
        const original = owner('Original');
        const replacement = owner('Replacement');
        expect(replacement.controller.opened).toBe(false);

        await replacement.controller.receive({
            role: 'program',
            requestId: 'reconnect',
            context: original.controller.state.context,
            version: original.controller.state.version,
            action: { kind: 'ready' },
        });

        expect(replacement.controller.opened).toBe(true);
        expect(replacement.controller.state.context).not.toBe(original.controller.state.context);
        expect(replacement.document.draft.dirty).toBe(false);
    });

    it('accepts a replacement owner with a lower version and ignores retired owner messages', async () => {
        const original = owner('Original');
        original.document.draft.set('level', 45);
        original.controller.refresh(original.document);
        original.document.draft.set('level', 46);
        original.controller.refresh(original.document);
        const oldState = original.controller.state;
        const { view, publish } = await windowFixture(original.controller);
        const replacement = owner('Replacement');
        expect(replacement.controller.state.version).toBeLessThan(oldState.version);

        await act(() => publish({ state: replacement.controller.state }));
        expect(view.getByRole('heading', { name: 'Mapping Editor: Replacement' })).toBeTruthy();

        await act(() => publish({ state: { ...oldState, version: oldState.version + 100 } }));
        expect(view.getByRole('heading', { name: 'Mapping Editor: Replacement' })).toBeTruthy();

        await act(() =>
            publish({
                state: {
                    ...replacement.controller.state,
                    title: 'Out of order',
                    version: replacement.controller.state.version - 1,
                },
            }),
        );
        expect(view.getByRole('heading', { name: 'Mapping Editor: Replacement' })).toBeTruthy();
    });

    it('keeps one owner identity when a selected image is closed', async () => {
        const { controller } = owner('Original');
        const populated = controller.state;
        controller.refresh(null);
        expect(controller.state).toEqual(expect.objectContaining({ owner: expect.any(String) }));
        expect(controller.state.owner).toBe(populated.owner);
        expect(controller.state.version).toBeGreaterThan(populated.version);
        expect(controller.state.context).toBe('');
    });

    it('heartbeats without locking edits or acknowledging a different pending action', async () => {
        const { controller, document } = owner('Original');
        document.draft.set('level', 40);
        controller.refresh(document);
        vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
        const { view, commands, stop, publish } = await windowFixture(controller);

        await act(() => vi.advanceTimersByTime(5000));
        const heartbeat = commands.at(-1)!;
        expect(heartbeat.action.kind).toBe('ready');
        expect(view.getByRole('button', { name: /^Save$/ })).toHaveProperty('disabled', false);

        await fireEvent.click(view.getByRole('button', { name: /^Save$/ }));
        const save = commands.find((command) => command.action.kind === 'save');
        expect(save).toBeDefined();
        expect(heartbeat.requestId).not.toBe(save!.requestId);
        await act(() => publish({ state: controller.state, requestId: heartbeat.requestId }));
        expect(view.getByRole('button', { name: /^Save$/ })).toHaveProperty('disabled', true);

        view.unmount();
        expect(stop).toHaveBeenCalledOnce();
        const commandCount = commands.length;
        await vi.advanceTimersByTimeAsync(10000);
        expect(commands).toHaveLength(commandCount);
    });
});
