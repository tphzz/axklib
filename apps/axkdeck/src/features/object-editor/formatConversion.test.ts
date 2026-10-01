import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { sampleConversionFixture, sampleFormatFixture } from '../../test/sampleFormatFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';

function setup(bank = false) {
    const detail = {
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: bank ? 'Bank' : 'Sample', type: bank ? 'SBAC' : 'SBNK' },
        formatConversion: sampleConversionFixture('A3000_188'),
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            partitionIndex: 0,
            volumeName: 'Volume',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            blockedParameters: [],
            blockedParameterReasons: {},
            unavailableParameters: {},
            ...sampleFormatFixture('A3000_188'),
        },
    } as unknown as ObjectDetail;
    const converted = structuredClone(detail);
    Object.assign(converted.editing!, sampleFormatFixture('A4000_A5000_224'), { payloadSha256: 'b'.repeat(64) });
    converted.formatConversion = sampleConversionFixture('A4000_A5000_224', { payloadSha256: 'b'.repeat(64) });
    if (bank) {
        detail.editing = null;
        converted.editing = null;
    }
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
describe('explicit Sample format conversion', () => {
    it.each(['clear', 'close'])('does not publish a delayed bank reload after %s', async (action) => {
        const { workflow, transport, detail } = setup(true);
        const document = (await workflow.load(1, 'sample'))!;
        let resolve!: (detail: ObjectDetail) => void;
        transport.objectDetail.mockReturnValueOnce(
            new Promise<ObjectDetail>((done) => {
                resolve = done;
            }),
        );
        const opening = workflow.openConversion(1, 'sample');
        await vi.waitFor(() => expect(transport.objectDetail).toHaveBeenCalledTimes(2));
        if (action === 'clear') workflow.clear();
        else workflow.closeConversion();
        const stale = structuredClone(detail);
        stale.formatConversion!.payloadSha256 = 'c'.repeat(64);
        resolve(stale);
        await opening;
        expect(workflow.conversionDocument).toBeNull();
        expect(document.detail).toBe(detail);
        if (action === 'clear') expect(workflow.documents).toEqual([]);
    });
    it('does not replace a newer bank recovery dialog with an earlier delayed reload', async () => {
        const { workflow, transport, detail } = setup(true);
        const first = (await workflow.load(1, 'sample'))!;
        let resolve!: (detail: ObjectDetail) => void;
        transport.objectDetail.mockReturnValueOnce(
            new Promise<ObjectDetail>((done) => {
                resolve = done;
            }),
        );
        const opening = workflow.openConversion(1, 'sample');
        await vi.waitFor(() => expect(transport.objectDetail).toHaveBeenCalledTimes(2));
        const other = structuredClone(detail);
        other.object = { ...other.object, id: 'other', key: 'other', name: 'Other bank' };
        transport.objectDetail.mockResolvedValue(other);
        await workflow.openConversion(1, 'other');
        const active = workflow.conversionDocument!;
        active.phase = 'refresh-failed';
        resolve(detail);
        await opening;
        expect(workflow.conversionDocument).toBe(active);
        expect(active).not.toBe(first);
        expect(workflow.locked).toBe(true);
    });
    it('converts only the bank without creating a parameter editor or resetting Sample navigation', async () => {
        const { workflow, transport } = setup(true);
        const navigation = workflow.navigation('a-series/sample');
        await workflow.openConversion(1, 'sample');
        const document = workflow.conversionDocument!;
        expect(document.canSave).toBe(false);
        expect(document.detail!.editing).toBeNull();
        await workflow.convert(document, 'A4000_A5000_224');
        expect(transport.startObjectFormatConversion).toHaveBeenCalledWith(
            1,
            expect.objectContaining({
                operation: expect.objectContaining({ type: 'convert_sbac_format', sample_bank_name: 'Bank' }),
            }),
        );
        expect(transport.startObjectFormatConversion.mock.calls[0]![1].operation).not.toHaveProperty('sample_name');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(workflow.navigation('a-series/sample')).toBe(navigation);
        expect(document.detail!.formatConversion!.sampleFormat.format).toBe('A4000_A5000_224');
        expect(document.status).toBe('Sample Bank format converted');
        expect(workflow.locked).toBe(false);
        expect(workflow.conversionDocument).toBeNull();
    });
    it('keeps a committed bank conversion recovery-only and reloads a stale bank when reopened', async () => {
        const { workflow, transport, refresh, detail } = setup(true);
        await workflow.openConversion(1, 'sample');
        const document = workflow.conversionDocument!;
        const changed = structuredClone(detail);
        changed.formatConversion!.payloadSha256 = 'c'.repeat(64);
        transport.objectDetail.mockResolvedValue(changed);
        await workflow.convert(document, 'A4000_A5000_224');
        expect(document.conflict).toContain('Close and reopen');
        expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
        workflow.closeConversion();
        await workflow.openConversion(1, 'sample');
        expect(document.conflict).toBe('');
        refresh.mockRejectedValueOnce(new Error('offline'));
        await workflow.convert(document, 'A4000_A5000_224');
        expect(document.phase).toBe('refresh-failed');
        workflow.closeConversion();
        expect(workflow.conversionDocument).toBe(document);
        await workflow.recover(document);
        expect(transport.startObjectFormatConversion).toHaveBeenCalledTimes(1);
        expect(workflow.conversionDocument).toBeNull();
    });
    it('requires a clean draft and never converts as part of ordinary saving', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'sample'))!;
        document.draft.set('level', 80);
        await workflow.convert(document, 'A4000_A5000_224');
        expect(workflow.conversionReason(document)).toContain('Save or discard');
        expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
    });
    it('retains document identity and navigation while accepting the converted baseline', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'sample'))!;
        const navigation = workflow.navigation('a-series/sample');
        await workflow.openConversion(1, 'sample');
        await workflow.convert(document, 'A4000_A5000_224');
        expect(transport.startObjectFormatConversion).toHaveBeenCalledWith(1, {
            expectedRevision: 1,
            operation: {
                id: 'sample-format',
                type: 'convert_sbnk_format',
                target_format: 'a4000_a5000_224',
                partition_index: 0,
                volume_name: 'Volume',
                sample_name: 'Sample',
                expected_payload_sha256: 'a'.repeat(64),
            },
        });
        expect(await workflow.load(1, 'sample')).toBe(document);
        expect(workflow.navigation('a-series/sample')).toBe(navigation);
        expect(document.detail!.editing!.sampleFormat.format).toBe('A4000_A5000_224');
        expect(document.draft.dirty).toBe(false);
        expect(document.phase).toBe('editable');
        expect(workflow.conversionDocument).toBeNull();
        document.draft.set('level', 90);
        expect(document.canSave).toBe(true);
    });
    it('rechecks blockers and refuses a changed source', async () => {
        for (const stale of [false, true]) {
            const { workflow, transport, detail } = setup();
            const document = (await workflow.load(1, 'sample'))!;
            const changed = structuredClone(detail);
            if (stale) changed.editing!.payloadSha256 = 'c'.repeat(64);
            else changed.formatConversion!.formatConversions[0]!.allowed = false;
            transport.objectDetail.mockResolvedValue(changed);
            await workflow.convert(document, 'A4000_A5000_224');
            expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
            expect(document.phase).toBe('editable');
        }
    });
    it('recovers a confirmed write with refresh only and an uncertain write by job identity', async () => {
        for (const stage of ['refresh', 'job']) {
            const { workflow, transport, refresh } = setup();
            const document = (await workflow.load(1, 'sample'))!;
            if (stage === 'refresh') refresh.mockRejectedValueOnce(new Error('offline'));
            else transport.waitForJob.mockRejectedValueOnce(new Error('offline'));
            await workflow.convert(document, 'A4000_A5000_224');
            expect(workflow.locked).toBe(true);
            await workflow.convert(document, 'A4000_A5000_224');
            await workflow.recover(document);
            expect(transport.startObjectFormatConversion).toHaveBeenCalledTimes(1);
            expect(workflow.locked).toBe(false);
        }
    });
});
