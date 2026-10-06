import { render, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../../../lib/transport';
import { mappingDetail } from '../../../../test/mappingEditorFixture';
import { objectEditors } from '../../../object-editor/context';
import { ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import SampleMapping from '../sample/SampleMapping.svelte';
import { BankDraft } from './draft.svelte';
import BankPreview from './BankPreview.svelte';

vi.mock('../../../object-editor/context', () => ({ objectEditors: vi.fn() }));

describe('composed bank preview and mapping', () => {
    it('deduplicates delayed revalidation, keeps dirty values and hides stale compact limits on failure', async () => {
        const transport = {
            objectDetail: vi.fn(async (_: number, id: string) => mappingDetail(id)),
            startObjectParameterEdit: vi.fn(async () => ({ jobId: 1, kind: 'edit', status: 'queued' as const })),
            waitForJob: vi.fn(async () => ({ jobId: 1, kind: 'edit', status: 'completed' as const })),
        };
        const workflow = new ObjectEditorWorkflow({
            transport,
            refresh: vi.fn(),
            stopPlayback: vi.fn(),
            status: vi.fn(),
        });
        const bank = (await workflow.load(1, 'bank'))!,
            member = (await workflow.load(1, 'a'))!;
        member.draft.set('key_low', 24);
        member.draft.set('level', 72);
        bank.detail = mappingDetail('bank', 2);
        bank.previewMemberId = 'a';
        let complete!: (detail: ObjectDetail) => void;
        transport.objectDetail.mockClear();
        transport.objectDetail.mockImplementation(async (_, id) =>
            id === 'a'
                ? new Promise<ObjectDetail>((resolve) => {
                      complete = resolve;
                  })
                : mappingDetail(id, 2),
        );
        vi.mocked(objectEditors).mockReturnValue(workflow);
        render(BankPreview, { document: bank, onready: vi.fn() });
        const compact = render(SampleMapping, { document: bank, disabled: false });
        await waitFor(() => expect(complete).toBeTypeOf('function'));
        expect(compact.container.querySelector('.compact-limits')).toBeNull();
        expect((bank.draft as BankDraft).member).toEqual({});
        member.draft.set('level', 77);
        complete(mappingDetail('a', 2));
        await waitFor(() => expect(bank.previewStatus).toBe(''));
        expect((bank.draft as BankDraft).member).toMatchObject({ key_low: 24, level: 77 });
        expect(bank.previewDetail?.image.revision).toBe(2);
        await waitFor(() =>
            expect(compact.container.querySelector<HTMLElement>('.compact-limits')?.style.left).toBe('18.75%'),
        );
        expect(transport.objectDetail.mock.calls.filter(([, id]) => id === 'a')).toHaveLength(1);

        transport.objectDetail.mockImplementation(async (_, id) => {
            if (id === 'a') throw new Error('Preview a is offline');
            return mappingDetail(id, 3);
        });
        bank.detail = mappingDetail('bank', 3);
        await waitFor(() => expect(bank.previewStatus).toContain('offline'));
        expect(compact.container.querySelector('.compact-limits')).toBeNull();
        expect(bank.previewDetail).toBeNull();
        expect((bank.draft as BankDraft).member).toEqual({});
        expect(member.draft.changes).toEqual({ key_low: 24, level: 77 });
    });
});
