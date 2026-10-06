import { describe, expect, it, vi } from 'vitest';
import { mappingDetail } from '../../../../test/mappingEditorFixture';
import { ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import { loadMappingMember, loadMappingMembers } from './mappingLoad';

async function setup() {
    const detail = mappingDetail('bank');
    const ids = Array.from({ length: 12 }, (_, index) => `member-${index}`);
    if (detail.editing?.profile === 'a-series/sample-bank')
        detail.editing.bankOverrides!.members = ids.map((id) => ({ objectId: id, name: id }));
    detail.relationships = ids.map((id, index) => ({
        ...detail.relationships[0]!,
        id: `slot-${index}`,
        targetObject: { ...detail.relationships[0]!.targetObject!, id },
    }));
    let active = 0,
        peak = 0;
    const pending: (() => void)[] = [];
    const transport = {
        startObjectParameterEdit: vi.fn(async () => ({ jobId: 1, kind: 'edit', status: 'queued' as const })),
        waitForJob: vi.fn(async () => ({ jobId: 1, kind: 'edit', status: 'completed' as const })),
        objectDetail: vi.fn(async (_: number, id: string) => {
            if (id === 'bank') return detail;
            active++;
            peak = Math.max(peak, active);
            await new Promise<void>((resolve) => pending.push(resolve));
            active--;
            return mappingDetail(id);
        }),
    };
    const workflow = new ObjectEditorWorkflow({ transport, refresh: vi.fn(), stopPlayback: vi.fn(), status: vi.fn() });
    const bank = (await workflow.load(1, 'bank'))!;
    return { workflow, bank, transport, pending, peak: () => peak };
}
describe('shared mapping member loading', () => {
    it('deduplicates compact/window reads, bounds concurrency and retains canonical drafts', async () => {
        const { workflow, bank, transport, pending, peak } = await setup();
        const jobs = Promise.all([
            loadMappingMembers(workflow, bank, () => true),
            loadMappingMembers(workflow, bank, () => true),
            loadMappingMember(workflow, bank, 'member-0'),
        ]);
        for (let batch = 0; batch < 3; batch++) {
            await vi.waitFor(() => expect(pending).toHaveLength(4));
            pending.splice(0).forEach((resolve) => resolve());
        }
        await jobs;
        expect(peak()).toBe(4);
        expect(transport.objectDetail).toHaveBeenCalledTimes(13);
        const member = workflow.find(1, 'member-0')!;
        member.draft.set('key_low', 24);
        await loadMappingMembers(workflow, bank, () => true);
        expect(transport.objectDetail).toHaveBeenCalledTimes(13);
        expect(workflow.find(1, 'member-0')).toBe(member);
        expect(member.draft.values.key_low).toBe(24);
    });
    it('stops scheduling new batches after the visible selection changes', async () => {
        const { workflow, bank, transport, pending } = await setup();
        let current = true;
        const job = loadMappingMembers(workflow, bank, () => current);
        await vi.waitFor(() => expect(pending).toHaveLength(4));
        current = false;
        pending.splice(0).forEach((resolve) => resolve());
        await job;
        expect(transport.objectDetail).toHaveBeenCalledTimes(5);
    });
    it('shares the same read limit with a preview outside the current batch', async () => {
        const { workflow, bank, transport, pending, peak } = await setup();
        let completed = false;
        const jobs = Promise.all([
            loadMappingMembers(workflow, bank, () => true),
            loadMappingMember(workflow, bank, 'member-8'),
        ]).then(() => {
            completed = true;
        });
        for (let round = 0; round < 12 && !completed; round++) {
            await vi.waitFor(() => expect(pending.length > 0 || completed).toBe(true));
            pending.splice(0).forEach((resolve) => resolve());
        }
        await jobs;
        expect(peak()).toBe(4);
        expect(transport.objectDetail).toHaveBeenCalledTimes(13);
        await expect(loadMappingMember(workflow, bank, 'unconfirmed')).rejects.toThrow('not a confirmed member');
        expect(transport.objectDetail).toHaveBeenCalledTimes(13);
    });
});
