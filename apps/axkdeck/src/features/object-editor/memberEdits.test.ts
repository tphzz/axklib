import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import type { ObjectParameterEdit } from '../../lib/objectEditing';
import { mappingDetail } from '../../test/mappingEditorFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';
import { BankDraft } from '../devices/a-series/bank/draft.svelte';

async function setup() {
    let revision = 1;
    const saved = new Map<string, Record<string, unknown>>();
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => {
            const detail = mappingDetail(id, revision);
            if (saved.has(id) && detail.editing?.profile !== 'a-series/program')
                detail.editing!.parameters = { ...detail.editing!.parameters, ...saved.get(id) };
            return detail;
        }),
        startObjectParameterEdit: vi.fn(async (_: number, edit: ObjectParameterEdit) => {
            for (const operation of edit.operations)
                if (operation.type === 'update_sbnk_parameters') saved.set(operation.sample_name, operation.parameters);
            revision++;
            return { jobId: 7, status: 'queued' as const, kind: 'edit' };
        }),
        waitForJob: vi.fn(async () => ({ jobId: 7, status: 'completed' as const, kind: 'edit' })),
    };
    const refresh = vi.fn(async () => {});
    const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
    const bank = (await workflow.load(1, 'bank'))!,
        a = (await workflow.load(1, 'a'))!,
        b = (await workflow.load(1, 'b'))!;
    a.draft.set('key_low', 10);
    b.draft.set('key_high', 110);
    return { workflow, transport, refresh, bank, a, b };
}
describe('atomic member edits', () => {
    it('does not repeat a submission whose response was lost before a job ID arrived', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        transport.startObjectParameterEdit.mockRejectedValueOnce(new Error('Response lost'));
        await workflow.members.save(bank);
        expect(workflow.locked).toBe(true);
        expect([bank.phase, a.phase, b.phase]).toEqual(['unconfirmed', 'unconfirmed', 'unconfirmed']);
        expect(bank.jobId).toBeNull();
        await workflow.recover(bank);
        await workflow.members.save(bank);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        expect(transport.waitForJob).not.toHaveBeenCalled();
    });
    it('publishes no partial refresh when the last saved member cannot be retrieved', async () => {
        const { workflow, transport, refresh, bank, a, b } = await setup();
        const read = transport.objectDetail.getMockImplementation()!;
        refresh.mockImplementationOnce(async () => {
            transport.objectDetail.mockImplementation(async (session, id) => {
                if (id === 'b') throw new Error('Last member unavailable');
                return read(session, id);
            });
        });
        await workflow.members.save(bank);
        expect(a.draft.dirty && b.draft.dirty).toBe(true);
        expect([bank.phase, a.phase, b.phase]).toEqual(['refresh-failed', 'refresh-failed', 'refresh-failed']);
        transport.objectDetail.mockImplementation(read);
        await workflow.recover(bank);
        expect(a.draft.dirty || b.draft.dirty).toBe(false);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
    });
    it('discards only changed confirmed members and preserves bank and unrelated drafts', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        const other = (await workflow.load(1, 'other'))!;
        bank.draft.set('level', 80);
        other.draft.set('level', 90);
        await workflow.members.discard(bank);
        expect(a.draft.dirty || b.draft.dirty).toBe(false);
        expect(bank.draft.dirty && other.draft.dirty).toBe(true);
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
    it('saves distinct members in one manifest and preserves excluded drafts and history', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        const other = (await workflow.load(1, 'other'))!;
        other.draft.set('level', 90);
        (bank.draft as BankDraft).set('level', 80);
        await workflow.members.save(bank);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        const edit = transport.startObjectParameterEdit.mock.calls[0]![1];
        expect(edit.expectedRevision).toBe(1);
        expect(edit.operations).toHaveLength(2);
        expect(new Set(edit.operations.map((operation) => operation.id)).size).toBe(2);
        expect(a.draft.dirty || b.draft.dirty).toBe(false);
        expect(bank.draft.changes).toEqual({ level: 80, '$bankOverride.33': true });
        expect(bank.draft.canUndo && other.draft.canUndo).toBe(true);
        expect(workflow.locked).toBe(false);
    });
    it('recovers every participant together without a second write or partial draft acceptance', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        transport.waitForJob.mockRejectedValueOnce(new Error('Disconnected'));
        await workflow.members.save(bank);
        expect([bank.phase, a.phase, b.phase]).toEqual(['unconfirmed', 'unconfirmed', 'unconfirmed']);
        transport.objectDetail.mockRejectedValueOnce(new Error('Refresh failed'));
        await workflow.recover(a);
        expect([a.phase, b.phase]).toEqual(['refresh-failed', 'refresh-failed']);
        expect(a.draft.dirty && b.draft.dirty).toBe(true);
        await workflow.recover(b);
        expect(a.draft.dirty || b.draft.dirty).toBe(false);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
    });
    it('rejects mixed preflight revisions and keeps all drafts', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        transport.objectDetail.mockImplementation(async (_, id) => mappingDetail(id, id === 'b' ? 2 : 1));
        await workflow.members.save(bank);
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(bank.status).toMatch(/revision/i);
        expect(a.draft.dirty && b.draft.dirty).toBe(true);
        expect(workflow.locked).toBe(false);
    });
    it('rejects removed or unconfirmed members before any write', async () => {
        const { workflow, transport, bank } = await setup();
        transport.objectDetail.mockImplementation(async (_, id) => {
            const detail = mappingDetail(id);
            if (id === 'bank') detail.relationships[0]!.quality = 'LIKELY';
            if (id === 'bank') detail.relationships.pop();
            return detail;
        });
        await workflow.members.save(bank);
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(bank.status).toMatch(/membership/i);
    });
    it('freezes every participant before preflight and ignores delayed background checks', async () => {
        const { workflow, transport, bank, a, b } = await setup();
        let resolve!: (detail: ObjectDetail) => void;
        transport.objectDetail.mockImplementationOnce(
            () =>
                new Promise((done) => {
                    resolve = done;
                }),
        );
        const check = workflow.check(a);
        const save = workflow.members.save(bank);
        expect([bank.phase, a.phase, b.phase]).toEqual(['saving', 'saving', 'saving']);
        await save;
        const accepted = a.detail;
        resolve(mappingDetail('a'));
        await check;
        expect(a.detail).toBe(accepted);
        expect(a.conflict).toBe('');
    });
});
