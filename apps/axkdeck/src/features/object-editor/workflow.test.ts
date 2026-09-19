import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { ObjectEditorWorkflow } from './workflow.svelte';

function detail(id = 'sample', revision = 1): ObjectDetail {
    return {
        image: { revision },
        object: { id, key: id, name: id },
        editing: {
            profile: 'a4000-a5000/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100, pan: 0, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 100 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            blockedParameters: [],
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
}
function setup() {
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => detail(id)),
        startObjectParameterEdit: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 7, status: 'completed' }),
    };
    const refresh = vi.fn().mockResolvedValue(undefined);
    return {
        transport,
        refresh,
        workflow: new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() }),
    };
}
describe('object editor lifecycle', () => {
    it('retains separate drafts, saves only changed fields, resets only the saved history', async () => {
        const { workflow, transport } = setup();
        const a = (await workflow.load(1, 'A'))!;
        const b = (await workflow.load(1, 'B'))!;
        a.draft.set('level', 80);
        b.draft.set('pan', 12);
        expect(await workflow.load(1, 'A')).toBe(a);
        await workflow.save(a);
        expect(transport.startObjectParameterEdit.mock.calls[0]![1].operation.parameters).toEqual({ level: 80 });
        expect(a.draft.canUndo).toBe(false);
        expect(b.draft.dirty).toBe(true);
    });
    it('rejects a stale payload even when object identity survived', async () => {
        const { workflow, transport } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('level', 80);
        const changed = detail('A', 2);
        changed.editing!.payloadSha256 = 'b'.repeat(64);
        transport.objectDetail.mockResolvedValue(changed);
        await workflow.save(a);
        expect(a.conflict).toContain('changed outside');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(a.draft.values.level).toBe(80);
    });
    it('permits a revision change only after checking the original payload and placement', async () => {
        const { workflow, transport } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('level', 80);
        transport.objectDetail.mockResolvedValue(detail('A', 2));
        await workflow.save(a);
        expect(transport.startObjectParameterEdit.mock.calls[0]![1].expectedRevision).toBe(2);
    });
    it('retries refresh without a second write', async () => {
        const { workflow, transport, refresh } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('level', 80);
        refresh.mockRejectedValueOnce(new Error('Offline'));
        await workflow.save(a);
        expect(a.phase).toBe('refresh-failed');
        await workflow.save(a);
        await workflow.recover(a);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        expect(a.phase).toBe('editable');
    });
    it('checks the acknowledged job after transport loss without resubmitting', async () => {
        const { workflow, transport } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('level', 80);
        transport.waitForJob.mockRejectedValueOnce(new Error('Offline'));
        await workflow.save(a);
        expect(a.phase).toBe('unconfirmed');
        await workflow.recover(a);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        expect(a.phase).toBe('editable');
    });
    it('validates the combined playback and loop draft', async () => {
        const { workflow } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('playback.length_frames', 50);
        expect(a.validation).toContain('Loop bounds');
        a.draft.set('loop_length_frames', 50);
        expect(a.validation).toBe('');
    });
    it('preserves the zero-loop sentinel outside a nonzero one-shot window', async () => {
        const { workflow } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('playback.start_frame', 10);
        a.draft.set('playback.length_frames', 90);
        a.draft.set('loop_length_frames', 0);
        expect(a.validation).toBe('');
        a.draft.set('loop_mode', 1);
        expect(a.validation).toContain('Loop bounds');
    });
    it('blocks invalid numeric input rather than saving the previous visible value', async () => {
        const { workflow } = setup();
        const a = (await workflow.load(1, 'A'))!;
        a.draft.set('level', Number.NaN);
        expect(a.canSave).toBe(false);
    });
});
