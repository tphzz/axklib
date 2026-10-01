import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { AxklibApiError } from '../../lib/httpErrors';
import { describe, expect, it, vi } from 'vitest';
import { sampleConversionFixture, sampleFormatFixture } from '../../test/sampleFormatFixture';
import type { ObjectDetail } from '../../lib/transport';
import type { SampleStorageFormat } from '../../lib/objectEditing';
import { ObjectEditorWorkflow } from './workflow.svelte';
import SampleFormatDialog from './SampleFormatDialog.svelte';
import SampleFormatBadge from './SampleFormatBadge.svelte';

async function setup(dirty = false, allowed = true, source: SampleStorageFormat = 'A3000_188', bank = false) {
    const format = sampleFormatFixture(source);
    const conversion = sampleConversionFixture(source);
    conversion.formatConversions[0]!.allowed = allowed;
    if (!allowed)
        conversion.formatConversions[0]!.blockers = [
            { key: 'coarse_tune', storedValue: 100, message: 'Outside the target range.' },
        ];
    const detail = {
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: 'Sample', type: bank ? 'SBAC' : 'SBNK' },
        formatConversion: conversion,
        editing: {
            ...format,
            profile: 'a-series/sample',
            parameters: { level: 100 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
        },
    } as unknown as ObjectDetail;
    if (bank) detail.editing = null;
    const transport = {
        objectDetail: vi.fn().mockResolvedValue(detail),
        startObjectParameterEdit: vi.fn(),
        startObjectFormatConversion: vi.fn(),
        waitForJob: vi.fn(),
    };
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn(),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    await workflow.openConversion(1, 'sample');
    const document = workflow.conversionDocument!;
    if (dirty) document.draft.set('level', 90);
    return { workflow, document, transport, view: render(SampleFormatDialog, { workflow, document }) };
}
describe('Sample format UI', () => {
    it('names the bank target and keeps pending-operation blockers visible with dismissal available', async () => {
        const { view, document } = await setup(false, true, 'A3000_188', true);
        expect(view.getByRole('dialog', { name: 'Convert to a4k/a5k sample bank format' }).textContent).toContain(
            'Member Samples and Wave Data are not converted or edited',
        );
        document.detail = {
            ...document.detail!,
            formatConversion: {
                ...document.detail!.formatConversion!,
                formatConversions: [
                    {
                        targetFormat: 'A4000_A5000_224',
                        allowed: false,
                        changes: [],
                        blockers: [
                            {
                                key: 'active_overrides',
                                storedValue: null,
                                message: 'Active bank overrides cannot be converted or discarded.',
                            },
                        ],
                    },
                ],
            },
        };
        await waitFor(() =>
            expect((view.getByRole('button', { name: /^Convert$/ }) as HTMLButtonElement).disabled).toBe(true),
        );
        expect(view.getByText(/Active bank overrides/)).toBeTruthy();
        expect((view.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(false);
    });
    it('uses one explicit confirmation without technical storage details or a checkbox', async () => {
        const { view, workflow } = await setup();
        const submit = vi.spyOn(workflow, 'convert').mockResolvedValue();
        const button = view.getByRole('button', { name: /^Convert$/ }) as HTMLButtonElement;
        expect(button.disabled).toBe(false);
        expect(view.queryByRole('checkbox')).toBeNull();
        const dialog = view.getByRole('dialog', { name: 'Convert to a4k/a5k sample format' });
        expect(dialog.textContent).not.toMatch(/bytes|prefix|extension|Do not treat/);
        expect(dialog.textContent).toContain('Wave Data remain unchanged');
        await fireEvent.click(button);
        expect(submit).toHaveBeenCalledOnce();
        expect(view.container.querySelector('.dialog-footer-actions .secondary-button')).not.toBeNull();
        expect(view.container.querySelector('.dialog-footer-actions .primary-button')).toBe(button);
    });
    it.each([
        [true, true],
        [false, false],
    ])('blocks conversion for dirty=%s allowed=%s', async (dirty, allowed) => {
        const { view } = await setup(dirty, allowed);
        expect(view.queryByRole('checkbox')).toBeNull();
        expect((view.getByRole('button', { name: /^Convert$/ }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.getByText(dirty ? /Save or discard/ : /Outside the target range/)).toBeTruthy();
    });
    it('names the reverse target and explains parameter-preserving conversion', async () => {
        const { view } = await setup(false, true, 'A4000_A5000_224');
        const dialog = view.getByRole('dialog', { name: 'Convert to a3k sample format' });
        expect(dialog.textContent).toContain('incompatible settings');
        expect(dialog.textContent).not.toMatch(/bytes|prefix|extension/);
    });
    it.each(['submission', 'job'] as const)('unlocks dismissal after a path conflict during %s', async (stage) => {
        const { view, workflow, document, transport } = await setup();
        const message = 'close open images and wait for active file operations to finish';
        if (stage === 'submission')
            transport.startObjectFormatConversion.mockRejectedValue(new AxklibApiError('entry_in_use', message, 409));
        else {
            transport.startObjectFormatConversion.mockResolvedValue({ jobId: 42, status: 'queued' });
            transport.waitForJob.mockResolvedValue({
                jobId: 42,
                status: 'failed',
                error: message,
                errorCode: 'entry_in_use',
            });
        }
        await fireEvent.click(view.getByRole('button', { name: /^Convert$/ }));
        await waitFor(() => expect(document.phase).toBe('editable'));
        expect(document.jobId).toBeNull();
        expect(document.conversionTarget).toBeNull();
        expect(workflow.locked).toBe(false);
        expect(view.getByRole('status').textContent).toContain('Conversion not started');
        expect((view.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        expect(workflow.conversionDocument).toBeNull();
    });
    it.each(['Close', 'Escape'])('allows %s after a confirmed conversion failure', async (action) => {
        const { view, workflow, document, transport } = await setup();
        transport.startObjectFormatConversion.mockResolvedValue({ jobId: 42, status: 'queued' });
        transport.waitForJob.mockResolvedValue({
            jobId: 42,
            status: 'failed',
            error: 'Image is in use',
            errorCode: 'entry_in_use',
        });
        await fireEvent.click(view.getByRole('button', { name: /^Convert$/ }));
        await waitFor(() => expect(document.status).toContain('Conversion not started'));
        if (action === 'Close') await fireEvent.click(view.getByRole('button', { name: 'Close' }));
        else await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        expect(workflow.conversionDocument).toBeNull();
        expect(workflow.locked).toBe(false);
    });
    it('keeps the stored-format badge when parameters need another generation', () => {
        const format = sampleFormatFixture('A3000_188').sampleFormat;
        format.parameterIssues = [{ key: 'aeg.attack_mode', storedValue: 2, message: 'Unsupported value' }];
        const view = render(SampleFormatBadge, { format });
        expect(view.getByText('a3k')).toBeTruthy();
        expect(view.queryByText('a4k/a5k')).toBeNull();
        expect(view.container.querySelector('.warning')).not.toBeNull();
    });
});
