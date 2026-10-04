import { describe, expect, it, vi } from 'vitest';
import { programEditorFixture } from '../../test/programEditorFixture';
import { MappingController } from './controller.svelte';
import type { MappingCommand } from './protocol';

function setup() {
    const workflow = { locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() };
    const controller = new MappingController(workflow);
    const { document } = programEditorFixture();
    controller.refresh(document);
    const command = (action: MappingCommand['action']): MappingCommand => ({
        requestId: '1',
        context: controller.state.context,
        version: controller.state.version,
        action,
    });
    return { controller, document, workflow, command };
}
describe('shared Program mapping draft', () => {
    it('applies one completed range gesture as one main-editor undo entry', async () => {
        const { controller, document, command } = setup();
        const baseline = { ...document.draft.values };
        await controller.receive(
            command({
                kind: 'range',
                assignmentId: 0,
                range: { low: 40, high: 80, velocityLow: 30, velocityHigh: 100 },
            }),
        );
        expect(document.draft.values['assignments.0.key_low']).toBe(40);
        document.draft.undo();
        expect(document.draft.values).toEqual(baseline);
        expect(document.draft.canUndo).toBe(false);
    });
    it('rejects delayed commands after another edit, a selection switch, or document removal', async () => {
        const { controller, document, command } = setup();
        const stale = command({
            kind: 'range',
            assignmentId: 0,
            range: { low: 40, high: 80, velocityLow: 30, velocityHigh: 100 },
        });
        document.draft.set('level', 30);
        expect((await controller.receive(stale)).error).toBeTruthy();
        expect(document.draft.values['assignments.0.key_low']).toBe(0);
        controller.refresh(programEditorFixture().document);
        expect((await controller.receive(stale)).error).toBeTruthy();
        controller.refresh(null);
        expect((await controller.receive(stale)).error).toBeTruthy();
    });
    it('preserves duplicate assignment identities and rejects invalid or blocked changes', async () => {
        const { controller, document, command } = setup();
        await controller.receive(command({ kind: 'select', assignmentId: 1 }));
        expect(document.programAssignmentId).toBe(1);
        expect(
            (
                await controller.receive(
                    command({
                        kind: 'range',
                        assignmentId: 0,
                        range: { low: 0, high: 127, velocityLow: 0, velocityHigh: 127 },
                    }),
                )
            ).error,
        ).toBeTruthy();
        expect(
            (
                await controller.receive(
                    command({
                        kind: 'range',
                        assignmentId: 1,
                        range: { low: 90, high: 20, velocityLow: 0, velocityHigh: 127 },
                    }),
                )
            ).error,
        ).toBeTruthy();
        document.phase = 'saving';
        expect((await controller.receive(command({ kind: 'undo' }))).error).toBeTruthy();
    });
    it('delegates save and recovery to the existing workflow, never writing itself', async () => {
        const { controller, document, workflow, command } = setup();
        document.draft.set('level', 33);
        controller.refresh(document);
        await controller.receive(command({ kind: 'save' }));
        expect(workflow.save).toHaveBeenCalledWith(document);
        document.phase = 'refresh-failed';
        controller.refresh(document);
        await controller.receive(command({ kind: 'recover' }));
        expect(workflow.recover).toHaveBeenCalledWith(document);
    });
});
