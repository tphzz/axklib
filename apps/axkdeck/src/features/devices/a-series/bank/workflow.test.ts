import { describe, expect, it, vi } from 'vitest';
import { ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import type { ObjectDetail } from '../../../../lib/transport';
import type { ObjectParameterEdit } from '../../../../lib/objectEditing';
import { BankDraft } from './draft.svelte';

describe('bank editing lifecycle', () => {
    it('saves only bank overrides, preserves the preview and navigation, and recovers without a second write', async () => {
        let active = false;
        let level = 100;
        let revision = 1;
        const detail = (): ObjectDetail =>
            ({
                image: { revision },
                object: { id: 'bank', name: 'Bank', key: 'bank', type: 'SBAC' },
                editing: {
                    ...sampleFormatFixture(),
                    profile: 'a-series/sample-bank',
                    editable: true,
                    reason: '',
                    partitionIndex: 0,
                    volumeName: 'Volume',
                    payloadSha256: String(revision).repeat(64),
                    parameters: { level, root_key: 67 },
                    blockedParameters: ['root_key'],
                    blockedParameterReasons: {},
                    unavailableParameters: {},
                    bankOverrides: {
                        units: [{ id: 33, keys: ['level'], selectors: [33], activeSelectors: active ? [33] : [] }],
                        members: [{ name: 'Member', objectId: 'member' }],
                    },
                    playbackWindow: { start_frame: 0, length_frames: 0 },
                    canEditPlayback: false,
                    maximumFrames: 0,
                    sources: [],
                    eqCoefficients: [0, 0, 8192, 0, 0],
                },
            }) as unknown as ObjectDetail;
        const transport = {
            objectDetail: vi.fn(async () => detail()),
            startObjectParameterEdit: vi.fn(async (_session: number, edit: ObjectParameterEdit) => {
                const operation = edit.operations[0]!;
                expect(operation.type).toBe('update_sample_bank_overrides');
                if (operation.type === 'update_sample_bank_overrides') {
                    expect(operation.sample_bank_name).toBe('Bank');
                    expect(operation.parameters).not.toHaveProperty('root_key');
                    expect(operation.enable).not.toContain(6);
                    expect(operation.disable).not.toContain(6);
                    active = operation.enable.includes(33);
                    level = Number(operation.parameters.level ?? level);
                }
                revision++;
                return { jobId: 1, kind: 'edit', status: 'queued' as const };
            }),
            waitForJob: vi.fn(async () => ({ jobId: 1, kind: 'edit', status: 'completed' as const })),
        };
        const refresh = vi.fn().mockRejectedValueOnce(new Error('Refresh unavailable'));
        const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
        const document = (await workflow.load(1, 'bank'))!;
        const draft = document.draft as BankDraft;
        draft.member = { level: 60, root_key: 44 };
        draft.set('root_key', 64);
        expect(draft.dirty).toBe(false);
        document.previewMemberId = 'member';
        const navigation = workflow.navigation('a-series/sample-bank');
        navigation.tab = 'filter';
        navigation.page = 'sample-eq';
        draft.set('level', 60);
        expect(document.canSave).toBe(true);
        await workflow.save(document);
        expect(document.phase).toBe('refresh-failed');
        await workflow.recover(document);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledTimes(1);
        expect(document.phase).toBe('editable');
        expect(draft.dirty).toBe(false);
        expect(document.previewMemberId).toBe('member');
        expect(navigation.page).toBe('sample-eq');
        expect(workflow.locked).toBe(false);
        draft.clearOverride('level');
        await workflow.save(document);
        expect(transport.startObjectParameterEdit.mock.calls[1]![1].operations[0]).toMatchObject({
            parameters: {},
            enable: [],
            disable: [33],
        });
        expect(draft.dirty).toBe(false);
        expect(draft.values.level).toBe(60);
        expect(draft.storedValues.root_key).toBe(67);
        expect(draft.values.root_key).toBe(44);
        const reopened = (await workflow.load(1, 'bank'))!;
        const reopenedDraft = reopened.draft as BankDraft;
        reopenedDraft.member = { level: 75, root_key: 48 };
        expect(reopenedDraft.storedValues.root_key).toBe(67);
        expect(reopenedDraft.values.root_key).toBe(48);
        expect(reopenedDraft.dirty).toBe(false);
    });
});
