import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { programConversionFixture } from '../../test/programFormatFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';
import ObjectFormatDialog from './ObjectFormatDialog.svelte';

async function setup(blocked = false) {
    const conversion = programConversionFixture(true);
    conversion.formatConversions[0]!.changes = ['Store the Program in A3000 V2 format.'];
    if (blocked) {
        conversion.formatConversions[0]!.allowed = false;
        conversion.formatConversions[0]!.blockers = [
            { key: 'effect.3', message: 'Effect 4 has settings that A3000 cannot retain.', storedValue: 20 },
        ];
    }
    const detail = {
        image: { revision: 1 },
        object: { id: 'program', key: 'program', name: '033', type: 'PROG' },
        editing: null,
        formatConversion: conversion,
    } as unknown as ObjectDetail;
    const transport = {
        objectDetail: vi.fn().mockResolvedValue(detail),
        startObjectParameterEdit: vi.fn(),
        startObjectFormatConversion: vi.fn(),
        waitForJob: vi.fn(),
    };
    const workflow = new ObjectEditorWorkflow({ transport, refresh: vi.fn(), stopPlayback: vi.fn(), status: vi.fn() });
    await workflow.openConversion(1, 'program');
    return { workflow, view: render(ObjectFormatDialog, { workflow, document: workflow.conversionDocument! }) };
}

describe('Program format dialog', () => {
    it('shows numeric identity, storage badge, scope and planned changes without a sample editor', async () => {
        const { workflow, view } = await setup();
        const dialog = view.getByRole('dialog', { name: 'Convert to a3k program format' });
        expect(dialog.textContent).toContain('033: Test');
        expect(dialog.textContent).toContain('System Files are not converted or edited');
        expect(view.getByText('a4k/a5k')).toBeTruthy();
        expect(view.getByText('Store the Program in A3000 V2 format.')).toBeTruthy();
        expect((view.getByRole('button', { name: /^Convert$/ }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.keyDown(dialog, { key: 'Escape' });
        expect(workflow.conversionDocument).toBeNull();
    });
    it('keeps loss blockers visible and never offers a forced conversion', async () => {
        const { workflow, view } = await setup(true);
        expect(view.getByText('Effect 4 has settings that A3000 cannot retain.')).toBeTruthy();
        expect(view.queryByText(/effect\.3/)).toBeNull();
        expect((view.getByRole('button', { name: /^Convert$/ }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.queryByRole('checkbox')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        await waitFor(() => expect(workflow.conversionDocument).toBeNull());
    });
});
