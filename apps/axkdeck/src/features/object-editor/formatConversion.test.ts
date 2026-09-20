import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';

function setup() {
    const detail = {
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: 'Sample' },
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
    const transport = {
        objectDetail: vi.fn().mockResolvedValue(detail),
        startObjectParameterEdit: vi.fn(),
        startSampleFormatConversion: vi.fn().mockResolvedValue({ jobId: 42, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 42, status: 'completed' }),
    };
    const refresh = vi.fn(async () => {
        transport.objectDetail.mockResolvedValue(converted);
    });
    const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
    return { detail, converted, transport, refresh, workflow };
}
describe('explicit Sample format conversion', () => {
    it('requires a clean draft and never converts as part of ordinary saving', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'sample'))!;
        document.draft.set('level', 80);
        await workflow.convert(document, 'A4000_A5000_224');
        expect(workflow.conversionReason(document)).toContain('Save or discard');
        expect(transport.startSampleFormatConversion).not.toHaveBeenCalled();
    });
    it('retains document identity and navigation while accepting the converted baseline', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'sample'))!;
        const navigation = workflow.navigation('a-series/sample');
        await workflow.openConversion(1, 'sample');
        await workflow.convert(document, 'A4000_A5000_224');
        expect(transport.startSampleFormatConversion).toHaveBeenCalledWith(1, {
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
            else changed.editing!.formatConversions[0]!.allowed = false;
            transport.objectDetail.mockResolvedValue(changed);
            await workflow.convert(document, 'A4000_A5000_224');
            expect(transport.startSampleFormatConversion).not.toHaveBeenCalled();
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
            expect(transport.startSampleFormatConversion).toHaveBeenCalledTimes(1);
            expect(workflow.locked).toBe(false);
        }
    });
});
