import { describe, expect, it, vi } from 'vitest';
import { mappingDetail } from '../../test/mappingEditorFixture';
import { ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import { BankDraft } from '../devices/a-series/bank/draft.svelte';
import { MappingController } from './controller.svelte';
import type { MappingAction } from './protocol';
import { sampleMappingPatch, sampleMappingRange } from '../devices/a-series/sample/mapping';

async function setup() {
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => mappingDetail(id)),
        startObjectParameterEdit: vi.fn(async () => ({ jobId: 8, kind: 'edit', status: 'queued' as const })),
        waitForJob: vi.fn(async () => ({ jobId: 8, kind: 'edit', status: 'completed' as const })),
    };
    const workflow = new ObjectEditorWorkflow({ transport, refresh: vi.fn(), stopPlayback: vi.fn(), status: vi.fn() });
    const bank = (await workflow.load(1, 'bank'))!,
        a = (await workflow.load(1, 'a'))!,
        b = (await workflow.load(1, 'b'))!;
    const bankWindow = new MappingController(workflow, 'bank'),
        members = new MappingController(workflow, 'members');
    bankWindow.refresh(bank);
    members.refresh(bank);
    const send = async (controller: MappingController, action: MappingAction) => {
        controller.refresh(controller.role === 'sample' ? a : bank);
        return controller.receive({
            role: controller.role,
            requestId: crypto.randomUUID(),
            context: controller.state.context,
            version: controller.state.version,
            action,
        });
    };
    return { workflow, transport, bank, a, b, bankWindow, members, send };
}
describe('Sample and Bank mapping contexts', () => {
    it('draws stored member ranges independently of active bank velocity overrides', async () => {
        const { bank, a, bankWindow, members } = await setup();
        a.draft.set('velocity_high', 84);
        const draft = bank.draft as BankDraft;
        draft.patch({ velocity_high: 126 });
        draft.patch({ velocity_high: 127 });
        bankWindow.refresh(bank);
        members.refresh(bank);
        expect(members.state.limits!.velocityHigh).toBe(84);
        expect(members.state.zones[0]!.velocityHigh).toBe(84);
        expect(members.state.zones[1]!.velocityHigh).toBe(127);
        expect(bankWindow.state.zones[0]!.velocityHigh).toBe(127);
        expect(draft.isOverridden('velocity_high')).toBe(true);
        expect(a.draft.values.velocity_high).toBe(84);
    });

    it('uses the current member for editing and undo after a cached selection change', async () => {
        const { bank, a, b, members, send } = await setup();
        const revision = members.state.editRevision;
        await send(members, { kind: 'select', selectionId: 1 });
        expect(bank.previewMemberId).toBe('b');
        expect(members.state.editRevision).toBe(revision);
        await send(members, {
            kind: 'range',
            selectionId: 1,
            boundaries: ['low'],
            range: { ...members.state.limits!, low: 20 },
        });
        expect(b.draft.values.key_low).toBe(20);
        expect(a.draft.values.key_low).toBe(0);
        await send(members, { kind: 'undo' });
        expect(b.draft.values.key_low).toBe(0);
        expect(a.draft.canUndo).toBe(false);
    });
    it('edits only a Sample root, preserving Orig sentinels and an independent undo action', async () => {
        const { workflow, a, send, bankWindow, members } = await setup();
        a.draft.accept({ ...a.draft.values, key_low: 255, key_high: 128 });
        const sample = new MappingController(workflow, 'sample');
        a.draft.beginGesture();
        a.draft.set('velocity_low', 5);
        await send(sample, { kind: 'root', selectionId: 0, note: 73 });
        expect(a.draft.changes).toEqual({ velocity_low: 5, root_key: 73 });
        expect(a.draft.values.key_low).toBe(255);
        expect(a.draft.values.key_high).toBe(128);
        expect(sample.state.limits).toMatchObject({ low: 73, high: 73 });
        await send(sample, { kind: 'undo' });
        expect(a.draft.changes).toEqual({ velocity_low: 5 });
        await send(sample, { kind: 'undo' });
        expect(a.draft.dirty).toBe(false);
        for (const controller of [bankWindow, members]) {
            expect(controller.state.rootEditable).toBe(false);
            expect((await send(controller, { kind: 'root', selectionId: 0, note: 72 })).error).toBeTruthy();
        }
    });
    it('rejects malformed, stale, wrong-selection and blocked root commands without changing the draft', async () => {
        const { workflow, a, send } = await setup();
        const sample = new MappingController(workflow, 'sample');
        for (const note of [128, -1, 60.5, NaN])
            expect((await send(sample, { kind: 'root', selectionId: 0, note })).error).toBeTruthy();
        expect((await send(sample, { kind: 'root', selectionId: 1, note: 72 })).error).toBeTruthy();
        const previous = { ...sample.state };
        a.draft.set('velocity_low', 1);
        expect(
            (
                await sample.receive({
                    role: 'sample',
                    requestId: 'stale',
                    context: previous.context,
                    version: previous.version,
                    action: { kind: 'root', selectionId: 0, note: 72 },
                })
            ).error,
        ).toBeTruthy();
        if (a.detail!.editing?.profile === 'a-series/sample') a.detail!.editing.blockedParameters = ['root_key'];
        expect((await send(sample, { kind: 'root', selectionId: 0, note: 72 })).error).toBeTruthy();
        expect(a.draft.changes).toEqual({ velocity_low: 1 });
    });
    it('shows a member-save preflight failure even when the selected member is clean', async () => {
        const { transport, b, members, send } = await setup();
        b.draft.set('key_low', 24);
        transport.objectDetail.mockImplementation(async (_, id) => {
            if (id === 'b') throw new Error('Cannot read member b');
            return mappingDetail(id);
        });
        await send(members, { kind: 'save' });
        expect(members.state.status).toContain('Cannot read member b');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
    it('retains Orig when an inline gesture returns to its original resolved key', () => {
        const values = { key_low: 255, key_high: 128, root_key: 60, velocity_low: 0, velocity_high: 127 };
        const range = sampleMappingRange(values)!;
        expect(sampleMappingPatch({ ...range, low: 59 }, ['low'], values)).toEqual({ key_low: 59 });
        expect(sampleMappingPatch(range, ['low'], values)).toEqual({ key_low: 255 });
    });
    it('preserves Orig and untouched bounds when editing Sample velocity or one key boundary', async () => {
        const { workflow, a, send } = await setup();
        a.draft.accept({ ...a.draft.values, key_low: 255, key_high: 128, velocity_low: 20, velocity_high: 80 });
        const sample = new MappingController(workflow, 'sample');
        sample.refresh(a);
        expect(sample.state.limits).toEqual({ low: 60, high: 60, velocityLow: 20, velocityHigh: 80 });
        expect(
            (
                await send(sample, {
                    kind: 'range',
                    selectionId: 0,
                    boundaries: ['velocityLow', 'velocityHigh'],
                    range: { low: 60, high: 60, velocityLow: 21, velocityHigh: 81 },
                })
            ).error,
        ).toBeUndefined();
        expect(a.draft.changes).toEqual({ velocity_low: 21, velocity_high: 81 });
        await send(sample, {
            kind: 'range',
            selectionId: 0,
            boundaries: ['low'],
            range: { low: 50, high: 60, velocityLow: 21, velocityHigh: 81 },
        });
        expect(a.draft.values.key_high).toBe(128);
        expect(a.draft.values.key_low).toBe(50);
    });
    it('shares canonical member drafts without projecting bank overrides into the stored member view', async () => {
        const { bank, a, bankWindow, members, send } = await setup();
        expect(bankWindow.state.editableAxes).toEqual({ keys: false, velocity: true });
        expect(
            (
                await send(bankWindow, {
                    kind: 'range',
                    selectionId: 0,
                    boundaries: ['low'],
                    range: { ...bankWindow.state.limits!, low: 1 },
                })
            ).error,
        ).toBeTruthy();
        await send(bankWindow, {
            kind: 'range',
            selectionId: 0,
            boundaries: ['velocityLow'],
            range: { ...bankWindow.state.limits!, velocityLow: 50 },
        });
        members.refresh(bank);
        expect(members.state.limits!.velocityLow).toBe(0);
        expect(members.state.zones[0]!.velocityLow).toBe(0);
        expect(bankWindow.state.zones[0]!.velocityLow).toBe(50);
        await send(members, {
            kind: 'range',
            selectionId: 0,
            boundaries: ['velocityLow'],
            range: { ...members.state.limits!, velocityLow: 20 },
        });
        expect(a.draft.values.velocity_low).toBe(20);
        expect(members.state.zones[0]!.velocityLow).toBe(20);
        bankWindow.refresh(bank);
        expect(bankWindow.state.zones[0]!.velocityLow).toBe(50);
        await send(bankWindow, { kind: 'inherit', boundary: 'velocityLow' });
        members.refresh(bank);
        expect(members.state.zones[0]!.velocityLow).toBe(20);
        expect((bank.draft as BankDraft).isOverridden('velocity_low')).toBe(false);
        await send(bankWindow, { kind: 'undo' });
        expect(a.draft.values.velocity_low).toBe(20);
    });
    it('keeps bank-only Save separate from member changes and rejects sibling-role commands', async () => {
        const { workflow, bank, a, b, bankWindow, members, send } = await setup();
        a.draft.set('key_low', 24);
        b.draft.set('velocity_high', 90);
        (bank.draft as BankDraft).set('level', 80);
        const save = vi.spyOn(workflow, 'save').mockResolvedValue();
        await send(bankWindow, { kind: 'save' });
        expect(save).toHaveBeenCalledExactlyOnceWith(bank);
        expect(a.draft.dirty && b.draft.dirty).toBe(true);
        expect(
            (
                await members.receive({
                    role: 'bank',
                    requestId: 'wrong',
                    context: members.state.context,
                    version: members.state.version,
                    action: { kind: 'undo' },
                })
            ).error,
        ).toMatch(/different/);
        expect(a.draft.values.key_low).toBe(24);
    });
    it('recovers the member transaction even when the selected member is clean', async () => {
        const { transport, bank, a, b, members, send } = await setup();
        b.draft.set('key_low', 24);
        transport.waitForJob.mockRejectedValueOnce(new Error('Lost connection'));
        await send(members, { kind: 'save' });
        expect(a.phase).toBe('editable');
        expect(bank.phase).toBe('unconfirmed');
        expect(members.state.recovery).toBe('Check status');
        await send(members, { kind: 'recover' });
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        expect(members.state.recovery).toBeNull();
    });
    it('does not grant editing through unresolved membership or blocked parameter axes', async () => {
        const { workflow, bank, a, members, send } = await setup();
        bank.detail!.relationships = [];
        members.refresh(bank);
        expect(members.state.selections).toHaveLength(0);
        expect(members.state.editable).toBe(false);
        if (a.detail!.editing?.profile === 'a-series/sample') a.detail!.editing.blockedParameters = ['key_low'];
        const sample = new MappingController(workflow, 'sample');
        sample.refresh(a);
        expect(sample.state.editableAxes.keys).toBe(false);
        expect(
            (
                await send(sample, {
                    kind: 'range',
                    selectionId: 0,
                    boundaries: ['low'],
                    range: { ...sample.state.limits!, low: 10 },
                })
            ).error,
        ).toBeTruthy();
    });
});
