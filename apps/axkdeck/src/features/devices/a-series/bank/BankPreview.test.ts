import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../../../lib/transport';
import { sampleSnapshot } from '../../../../lib/objectEditing';
import { mappingDetail } from '../../../../test/mappingEditorFixture';
import { objectEditors } from '../../../object-editor/context';
import { ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import SampleMapping from '../sample/SampleMapping.svelte';
import { BankDraft } from './draft.svelte';
import BankPreview from './BankPreview.svelte';

vi.mock('../../../object-editor/context', () => ({ objectEditors: vi.fn() }));

const originalScroll = Element.prototype.scrollIntoView;
beforeAll(() => {
    Element.prototype.scrollIntoView = vi.fn();
});
afterAll(() => {
    Element.prototype.scrollIntoView = originalScroll;
});
async function preview(empty = false, unavailable = false) {
    const detail = mappingDetail('bank');
    sampleSnapshot(detail)!.bankOverrides!.members = empty
        ? []
        : [
              { objectId: 'a', name: 'PB BRASS A' },
              { objectId: 'b', name: 'DNBS drum loop 1' },
              { objectId: null, name: 'Missing sample' },
              { objectId: 'unconfirmed', name: 'Unconfirmed sample' },
          ];
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => (id === 'bank' ? detail : mappingDetail(id))),
        startObjectParameterEdit: vi.fn(),
        waitForJob: vi.fn(),
    };
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn(),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    const bank = (await workflow.load(1, 'bank'))!;
    if (unavailable) bank.previewMemberId = 'removed';
    vi.mocked(objectEditors).mockReturnValue(workflow);
    const view = render(BankPreview, { document: bank, onready: vi.fn() });
    await waitFor(() => expect(bank.previewMemberId).not.toBeUndefined());
    return { view, bank, transport };
}

describe('bank preview dropdown', () => {
    it('uses the shared non-searchable dropdown and keeps member IDs out of display indices', async () => {
        const { view, bank } = await preview();
        const trigger = view.getByRole('button', { name: 'Preview sample' });
        expect(trigger.classList.contains('editor-select')).toBe(true);
        expect(trigger.textContent?.trim()).toBe('PB BRASS A');
        expect(trigger.title).toBe('PB BRASS A');
        expect(view.queryByRole('combobox')).toBeNull();
        await fireEvent.click(trigger);
        expect(view.getByRole('option', { name: 'PB BRASS A' }).getAttribute('aria-selected')).toBe('true');
        await fireEvent.click(view.getByRole('option', { name: 'DNBS drum loop 1' }));
        await waitFor(() => expect(bank.previewMemberId).toBe('b'));
        expect(trigger.textContent?.trim()).toBe('DNBS drum loop 1');
        expect(bank.draft.dirty).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Previous preview sample' }));
        expect(bank.previewMemberId).toBe('a');
        await fireEvent.click(view.getByRole('button', { name: 'Next preview sample' }));
        expect(bank.previewMemberId).toBe('b');
    });
    it('retains keyboard selection, Escape and outside dismissal without changing a draft', async () => {
        const { view, bank } = await preview();
        const trigger = view.getByRole('button', { name: 'Preview sample' });
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        let popup = view.getByRole('listbox');
        await fireEvent.keyDown(popup, { key: 'ArrowDown' });
        await fireEvent.keyDown(popup, { key: 'Enter' });
        expect(bank.previewMemberId).toBe('b');
        await fireEvent.click(trigger);
        popup = view.getByRole('listbox');
        await fireEvent.keyDown(popup, { key: 'Home' });
        await fireEvent.keyDown(popup, { key: 'Escape' });
        expect(bank.previewMemberId).toBe('b');
        expect(view.queryByRole('listbox')).toBeNull();
        expect(document.activeElement).toBe(trigger);
        await fireEvent.click(trigger);
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('listbox')).toBeNull();
        expect(bank.draft.dirty).toBe(false);
    });
    it('shows a disabled empty-bank label and never opens a popup', async () => {
        const { view, bank } = await preview(true);
        const trigger = view.getByRole('button', { name: 'Preview sample' });
        expect(trigger.textContent?.trim()).toBe('No samples');
        expect(trigger.matches(':disabled')).toBe(true);
        expect(view.getByRole('button', { name: 'Previous preview sample' }).matches(':disabled')).toBe(true);
        expect(view.getByRole('button', { name: 'Next preview sample' }).matches(':disabled')).toBe(true);
        await fireEvent.click(trigger);
        expect(view.queryByRole('listbox')).toBeNull();
        expect(bank.previewMemberId).toBeNull();
    });
    it('keeps unavailable selection and unresolved members visible but unselectable', async () => {
        const { view, bank, transport } = await preview(false, true);
        const trigger = view.getByRole('button', { name: 'Preview sample' });
        expect(trigger.textContent?.trim()).toBe('Unresolved member');
        await fireEvent.click(trigger);
        for (const name of ['Unresolved member', 'Missing sample (unresolved)', 'Unconfirmed sample (unresolved)']) {
            const option = view.getByRole('option', { name });
            expect(option.getAttribute('aria-disabled')).toBe('true');
            await fireEvent.click(option);
            expect(bank.previewMemberId).toBe('removed');
        }
        expect(transport.objectDetail.mock.calls.map(([, id]) => id)).toEqual(['bank']);
        await fireEvent.click(view.getByRole('option', { name: 'PB BRASS A' }));
        await waitFor(() => expect(bank.previewMemberId).toBe('a'));
        expect(bank.draft.dirty).toBe(false);
    });
});

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
