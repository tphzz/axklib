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
        role: 'program',
        requestId: '1',
        context: controller.state.context,
        version: controller.state.version,
        action,
    });
    return { controller, document, workflow, command };
}
describe('shared Program mapping draft', () => {
    it('admits notes after a selection-only update but refuses a stale image revision', async () => {
        const audio = { play: vi.fn().mockResolvedValue(undefined), release: vi.fn() };
        const controller = new MappingController(
            { locked: false, save: vi.fn(), discard: vi.fn(), recover: vi.fn() },
            'program',
            audio,
        );
        const { document } = programEditorFixture();
        controller.refresh(document);
        const command: MappingCommand = {
            role: 'program',
            requestId: 'note',
            context: controller.state.context,
            version: controller.state.version,
            action: {
                kind: 'note',
                clientId: 'child',
                sequence: 1,
                note: 60,
                velocity: 100,
                imageRevision: document.detail!.image.revision,
            },
        };
        document.programAssignmentId = 1;
        controller.refresh(document);
        expect((await controller.receive(command)).error).toBeUndefined();
        expect(audio.play).toHaveBeenCalledTimes(1);
        controller.refresh(null);
        controller.refresh(document);
        const stale = { ...command, context: controller.state.context };
        document.detail = {
            ...document.detail!,
            image: { ...document.detail!.image, revision: document.detail!.image.revision + 1 },
        };
        controller.refresh(document);
        expect((await controller.receive(stale)).error).toBeTruthy();
        expect(audio.play).toHaveBeenCalledTimes(1);
        controller.dispose();
    });
    it('keeps edit revision stable on selection and atomically moves an unselected target with one undo', async () => {
        const { controller, document, command } = setup();
        document.draft.patch({
            'assignments.1.key_low': 20,
            'assignments.1.key_high': 40,
            'assignments.1.velocity_low': 30,
            'assignments.1.velocity_high': 60,
        });
        controller.refresh(document);
        const revision = controller.state.editRevision;
        await controller.receive(command({ kind: 'select', selectionId: 0 }));
        expect(controller.state.editRevision).toBe(revision);
        const baseline = { ...document.draft.values };
        expect(
            (
                await controller.receive(
                    command({
                        kind: 'move',
                        selectionId: 1,
                        editRevision: revision,
                        original: { low: 20, high: 40, velocityLow: 30, velocityHigh: 60 },
                        range: { low: 24, high: 44, velocityLow: 32, velocityHigh: 62 },
                        boundaries: ['low', 'high', 'velocityLow', 'velocityHigh'],
                    }),
                )
            ).error,
        ).toBeUndefined();
        expect(document.programAssignmentId).toBe(1);
        expect(document.draft.values['assignments.1.key_low']).toBe(24);
        expect(document.draft.values.root_key).toBe(baseline.root_key);
        document.draft.undo();
        expect(document.draft.values).toEqual(baseline);
    });
    it('applies one completed range gesture as one main-editor undo entry', async () => {
        const { controller, document, command } = setup();
        const baseline = { ...document.draft.values };
        await controller.receive(
            command({
                kind: 'range',
                selectionId: 0,
                boundaries: ['low', 'high', 'velocityLow', 'velocityHigh'],
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
            selectionId: 0,
            boundaries: ['low', 'high', 'velocityLow', 'velocityHigh'],
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
        await controller.receive(command({ kind: 'select', selectionId: 1 }));
        expect(document.programAssignmentId).toBe(1);
        expect(
            (
                await controller.receive(
                    command({
                        kind: 'range',
                        selectionId: 0,
                        boundaries: ['low', 'high', 'velocityLow', 'velocityHigh'],
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
                        selectionId: 1,
                        boundaries: ['low', 'high', 'velocityLow', 'velocityHigh'],
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
