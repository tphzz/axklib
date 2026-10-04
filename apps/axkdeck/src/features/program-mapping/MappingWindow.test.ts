import { act, fireEvent, render, waitFor } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import MappingWindow from './MappingWindow.svelte';
import { MappingController } from './controller.svelte';
import { programEditorFixture } from '../../test/programEditorFixture';
import type { MappingCommand, MappingMessage } from './protocol';
beforeEach(() => {
    HTMLElement.prototype.scrollIntoView = vi.fn();
});

async function setup() {
    const workflow = { locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() };
    const controller = new MappingController(workflow);
    const { document } = programEditorFixture();
    controller.refresh(document);
    let publish: (message: MappingMessage) => void = () => {};
    const stop = vi.fn();
    const commands: MappingCommand[] = [];
    const view = render(MappingWindow, {
        adapter: {
            listen: async (callback) => {
                publish = callback;
                return stop;
            },
            send: async (command) => {
                commands.push(command);
                publish(await controller.receive(command));
            },
        },
    });
    await waitFor(() => expect(view.getByRole('button', { name: 'Low key limit' })).toBeTruthy());
    return { view, document, controller, commands, stop, publish };
}
describe('Mapping Editor window', () => {
    it('shares keyboard edits, undo and duplicate assignment selection with the main draft', async () => {
        const { view, document, commands } = await setup();
        await fireEvent.keyDown(view.getByRole('button', { name: 'High velocity limit' }), { key: 'ArrowDown' });
        await waitFor(() => expect(document.draft.values['assignments.0.velocity_high']).toBe(126));
        expect(commands.filter((command) => command.action.kind === 'range')).toHaveLength(1);
        await fireEvent.click(view.getByRole('button', { name: 'Undo' }));
        await waitFor(() => expect(document.draft.values['assignments.0.velocity_high']).toBe(127));
        await fireEvent.click(view.getByRole('button', { name: 'Sample/Bank' }));
        await fireEvent.click(view.getByRole('option', { name: 'Duplicate (Sample) · 2' }));
        expect(document.programAssignmentId).toBe(1);
    });
    it('follows main selection changes, clears closed images, and disposes its listener', async () => {
        const { view, controller, publish, stop } = await setup();
        const next = programEditorFixture().document;
        const editing = next.detail!.editing;
        if (editing?.profile === 'a-series/program') editing.programName = 'Another Program';
        await act(() => {
            controller.refresh(next);
            publish({ state: controller.state });
        });
        expect(view.getByRole('heading', { name: 'Mapping Editor: Another Program' })).toBeTruthy();
        await act(() => {
            controller.refresh(null);
            publish({ state: controller.state });
        });
        expect(view.queryByRole('button', { name: 'Low key limit' })).toBeNull();
        expect(view.getByRole('button', { name: 'Save' })).toHaveProperty('disabled', true);
        view.unmount();
        expect(stop).toHaveBeenCalledOnce();
    });
});
