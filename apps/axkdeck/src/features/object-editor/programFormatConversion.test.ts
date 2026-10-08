import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { programConversionFixture } from '../../test/programFormatFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';

function setup() {
    const detail = {
        image: { revision: 1 },
        object: { id: 'program', key: 'program', name: '033', type: 'PROG' },
        editing: null,
        formatConversion: programConversionFixture(),
    } as unknown as ObjectDetail;
    const converted = structuredClone(detail);
    converted.formatConversion = programConversionFixture(true);
    const transport = {
        objectDetail: vi.fn().mockResolvedValue(detail),
        startObjectParameterEdit: vi.fn(),
        startObjectFormatConversion: vi.fn().mockResolvedValue({ jobId: 42, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 42, status: 'completed' }),
    };
    const refresh = vi.fn(async () => {
        transport.objectDetail.mockResolvedValue(converted);
    });
    const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
    return { detail, converted, transport, refresh, workflow };
}

describe('explicit Program format conversion', () => {
    it('converts only the numeric Program slot without opening a parameter editor', async () => {
        const { workflow, transport } = setup();
        await workflow.openConversion(1, 'program');
        const document = workflow.conversionDocument!;
        expect(document.canSave).toBe(false);
        await workflow.convert(document, 'A4000_A5000');
        expect(transport.startObjectFormatConversion).toHaveBeenCalledWith(1, {
            expectedRevision: 1,
            operation: {
                id: 'object-format',
                type: 'convert_prog_format',
                partition_index: 0,
                volume_name: 'Volume',
                program_number: 33,
                target_format: 'a4000_a5000',
                expected_payload_sha256: 'a'.repeat(64),
            },
        });
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(document.status).toBe('Program format converted');
        expect(workflow.conversionDocument).toBeNull();
    });
    it('rejects stale Program identity before submission and refreshes on reopening', async () => {
        const { workflow, transport, detail } = setup();
        await workflow.openConversion(1, 'program');
        const document = workflow.conversionDocument!;
        const changed = structuredClone(detail);
        changed.formatConversion = { ...programConversionFixture(), programNumber: 34 };
        transport.objectDetail.mockResolvedValue(changed);
        await workflow.convert(document, 'A4000_A5000');
        expect(document.conflict).toContain('Close and reopen');
        expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
        workflow.closeConversion();
        await workflow.openConversion(1, 'program');
        expect(document.detail!.formatConversion).toEqual(changed.formatConversion);
        expect(document.conflict).toBe('');
    });
    it('retains a committed conversion for refresh recovery without resubmitting', async () => {
        const { workflow, transport, refresh } = setup();
        await workflow.openConversion(1, 'program');
        const document = workflow.conversionDocument!;
        refresh.mockRejectedValueOnce(new Error('offline'));
        await workflow.convert(document, 'A4000_A5000');
        expect(document.phase).toBe('refresh-failed');
        workflow.closeConversion();
        expect(workflow.conversionDocument).toBe(document);
        await workflow.recover(document);
        expect(document.phase).toBe('editable');
        expect(transport.startObjectFormatConversion).toHaveBeenCalledTimes(1);
    });
    it('never submits an incompatible target or read-only Program', async () => {
        const { workflow, transport, detail } = setup();
        detail.formatConversion!.canConvertFormat = false;
        detail.formatConversion!.reason = 'Read-only image';
        await workflow.openConversion(1, 'program');
        await workflow.convert(workflow.conversionDocument!, 'A4000_A5000');
        expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
    });
});
