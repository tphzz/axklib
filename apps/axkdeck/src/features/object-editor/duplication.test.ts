import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { describe, expect, it, vi } from 'vitest';
import { AxklibApiError } from '../../lib/httpErrors';
import type { ObjectDetail } from '../../lib/transport';
import { ObjectEditorWorkflow } from './workflow.svelte';

function detail(id = 'Source', revision = 1): ObjectDetail {
    return {
        image: { revision },
        object: { id, key: id, name: id },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100, pan: 0, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 100 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
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
        startObjectParameterEdit: vi.fn().mockResolvedValue({ jobId: 5, status: 'queued' }),
        startSampleDuplication: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 7, status: 'completed' }),
    };
    const refresh = vi.fn().mockResolvedValue(undefined);
    const oncreated = vi.fn(async (_: string) => undefined);
    const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
    return { workflow, duplicate: workflow.duplication, transport, refresh, oncreated };
}

describe('Sample duplication workflow', () => {
    it('copies the current draft and playback changes without saving or clearing the source', async () => {
        const { workflow, duplicate, transport, refresh, oncreated } = setup();
        const source = (await workflow.load(1, 'Source'))!;
        source.draft.set('level', 80);
        source.draft.set('playback.start_frame', 10);
        source.draft.set('playback.length_frames', 90);
        source.draft.set('loop_start_frame', 10);
        source.draft.set('loop_length_frames', 80);
        const changes = { ...source.draft.changes };
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        expect(duplicate.source).toBe(source);
        duplicate.name = ' Copy ';

        await duplicate.submit();

        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(1);
        expect(transport.startSampleDuplication).toHaveBeenCalledWith(1, {
            expectedRevision: 1,
            operation: expect.objectContaining({
                id: expect.any(String),
                type: 'duplicate_sbnk',
                partition_index: 0,
                volume_name: 'Volume',
                sample_name: 'Source',
                new_name: 'Copy',
                expected_payload_sha256: 'a'.repeat(64),
                parameters: { level: 80, loop_start_frame: 10, loop_length_frames: 80 },
                playback_window: { start_frame: 10, length_frames: 90 },
            }),
        });
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(source.draft.changes).toEqual(changes);
        expect(source.draft.dirty).toBe(true);
        expect(source.draft.canUndo).toBe(true);
        expect(source.detail!.editing!.parameters.level).toBe(100);
        expect(source.phase).toBe('editable');
        expect(refresh).toHaveBeenCalledTimes(1);
        expect(oncreated).toHaveBeenCalledExactlyOnceWith('Copy');
        expect(duplicate.visible).toBe(false);
    });

    it('permits a clean source and produces an empty parameter patch', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        expect(duplicate.canSubmit).toBe(true);
        await duplicate.submit();
        const edit = transport.startSampleDuplication.mock.calls[0]![1];
        expect(edit.operation.parameters).toEqual({});
        expect(edit.operation).not.toHaveProperty('playback_window');
    });

    it('suggests a unique bounded name and validates trimmed ASCII names case-insensitively', async () => {
        const { duplicate, oncreated } = setup();
        const existing = ['Source', 'Source Copy', 'Copy', 'source copy 2', 'abcdefghijklmnop'];
        await duplicate.open(1, 'Source', existing, oncreated);
        expect(duplicate.name.trim()).not.toBe('');
        expect(duplicate.name.length).toBeLessThanOrEqual(16);
        expect(existing.map((name) => name.toLowerCase())).not.toContain(duplicate.name.toLowerCase());
        expect(duplicate.canSubmit).toBe(true);
        for (const name of ['', '   ', 'sOuRcE', '  cOpY ', 'abcdefghijklmnopq', 'Non-ASCII\u00e9', 'Line\nBreak']) {
            duplicate.name = name;
            expect(duplicate.validation, name).not.toBe('');
            expect(duplicate.canSubmit, name).toBe(false);
        }
        duplicate.name = '  Unique  ';
        expect(duplicate.validation).toBe('');
        expect(duplicate.canSubmit).toBe(true);
        duplicate.name = '1234567890123456';
        expect(duplicate.validation).toBe('');
    });

    it('blocks invalid draft values, input errors, conflicts, and unresolved source writes', async () => {
        const { workflow, duplicate, transport, oncreated } = setup();
        const source = (await workflow.load(1, 'Source'))!;
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        source.draft.set('level', Number.NaN);
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        source.draft.set('level', 80);
        source.inputErrors = { level: 'Enter a whole number' };
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        source.inputErrors = {};
        source.conflict = 'This Sample changed outside the editor';
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        source.conflict = '';
        source.phase = 'unconfirmed';
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
    });

    it('rejects unsupported or read-only sources without starting a write', async () => {
        const { duplicate, transport, oncreated } = setup();
        const readOnly = detail();
        readOnly.editing!.editable = false;
        readOnly.editing!.reason = 'Image is read-only';
        transport.objectDetail.mockResolvedValue(readOnly);
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        expect(duplicate.canSubmit).toBe(false);
        expect(`${duplicate.validation} ${duplicate.message}`).toContain('read-only');
        await duplicate.submit();
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
    });

    it('rejects asymmetric stereo playback layouts even when parameter editing is available', async () => {
        const { duplicate, transport, oncreated } = setup();
        const asymmetric = detail();
        asymmetric.editing!.canEditPlayback = false;
        transport.objectDetail.mockResolvedValue(asymmetric);
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        expect(duplicate.canSubmit).toBe(false);
        expect(`${duplicate.validation} ${duplicate.message}`).toMatch(/stereo|playback/i);
        await duplicate.submit();
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
        expect(oncreated).not.toHaveBeenCalled();
    });

    it('rechecks playback-layout support before admitting the duplicate write', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        expect(duplicate.canSubmit).toBe(true);
        const asymmetric = detail('Source', 2);
        asymmetric.editing!.canEditPlayback = false;
        transport.objectDetail.mockResolvedValue(asymmetric);
        await duplicate.submit();
        expect(duplicate.canSubmit).toBe(false);
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
        expect(oncreated).not.toHaveBeenCalled();
    });

    it('checks the source immediately before writing and uses the refreshed revision', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        let resolveCheck!: (value: ObjectDetail) => void;
        transport.objectDetail.mockReturnValueOnce(new Promise((resolve) => (resolveCheck = resolve)));
        const submitted = duplicate.submit();
        expect(duplicate.phase).toBe('checking');
        expect(duplicate.locked).toBe(true);
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
        resolveCheck(detail('Source', 3));
        await submitted;
        expect(transport.startSampleDuplication.mock.calls[0]![1].expectedRevision).toBe(3);
        expect(transport.objectDetail).toHaveBeenCalledWith(1, 'Source');
    });

    it('rejects source payload and placement changes before admitting duplication', async () => {
        for (const change of ['payload', 'volume', 'name', 'identity'] as const) {
            const { workflow, duplicate, transport, oncreated } = setup();
            const source = (await workflow.load(1, 'Source'))!;
            source.draft.set('level', 80);
            await duplicate.open(1, 'Source', ['Source'], oncreated);
            duplicate.name = 'Copy';
            const changed = detail('Source', 2);
            if (change === 'payload') changed.editing!.payloadSha256 = 'b'.repeat(64);
            if (change === 'volume') changed.editing!.volumeName = 'Elsewhere';
            if (change === 'name') changed.object.name = 'Renamed';
            if (change === 'identity') changed.object.key = 'replacement';
            transport.objectDetail.mockResolvedValue(changed);
            await duplicate.submit();
            expect(transport.startSampleDuplication, change).not.toHaveBeenCalled();
            expect(duplicate.canSubmit, change).toBe(false);
            expect(source.draft.values.level, change).toBe(80);
            expect(oncreated, change).not.toHaveBeenCalled();
        }
    });

    it('does not write when the preflight source check fails', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        transport.objectDetail.mockRejectedValueOnce(new Error('Source unavailable'));
        await duplicate.submit();
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
        expect(duplicate.phase).toBe('editable');
        expect(duplicate.message).toContain('Source unavailable');
    });

    it('preserves the source draft when backend validation rejects the write', async () => {
        const { workflow, duplicate, transport, oncreated } = setup();
        const source = (await workflow.load(1, 'Source'))!;
        source.draft.set('level', 80);
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        transport.startSampleDuplication.mockRejectedValueOnce(
            new AxklibApiError('transaction_stale', 'Sample changed before writing', 409),
        );
        await duplicate.submit();
        expect(duplicate.phase).toBe('editable');
        expect(duplicate.visible).toBe(true);
        expect(duplicate.message).toContain('Sample changed before writing');
        expect(source.draft.changes).toEqual({ level: 80 });
        expect(transport.waitForJob).not.toHaveBeenCalled();
        expect(oncreated).not.toHaveBeenCalled();
    });

    it('recovers a committed-write refresh failure without issuing another write', async () => {
        const { duplicate, transport, refresh, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        refresh.mockRejectedValueOnce(new Error('Refresh unavailable'));
        await duplicate.submit();
        expect(duplicate.phase).toBe('refresh-failed');
        expect(duplicate.visible).toBe(true);
        expect(duplicate.locked).toBe(true);
        expect(duplicate.canSubmit).toBe(false);
        expect(oncreated).not.toHaveBeenCalled();
        await duplicate.submit();
        await duplicate.recover();
        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(1);
        expect(transport.waitForJob).toHaveBeenCalledTimes(1);
        expect(refresh).toHaveBeenCalledTimes(2);
        expect(oncreated).toHaveBeenCalledExactlyOnceWith('Copy');
        expect(duplicate.visible).toBe(false);
    });

    it('recovers destination selection failure without writing a second duplicate', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        oncreated.mockRejectedValueOnce(new Error('New Sample not yet available'));
        await duplicate.submit();
        expect(duplicate.phase).toBe('refresh-failed');
        expect(duplicate.visible).toBe(true);
        await duplicate.recover();
        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(1);
        expect(oncreated).toHaveBeenCalledTimes(2);
        expect(duplicate.visible).toBe(false);
    });

    it('checks an acknowledged job after transport loss rather than resubmitting', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        transport.waitForJob.mockRejectedValueOnce(new Error('Connection lost'));
        await duplicate.submit();
        expect(duplicate.phase).toBe('unconfirmed');
        expect(duplicate.canCheck).toBe(true);
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        await duplicate.recover();
        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(1);
        expect(transport.waitForJob).toHaveBeenCalledTimes(2);
        expect(transport.waitForJob.mock.calls.map((args) => args[0])).toEqual([7, 7]);
        expect(oncreated).toHaveBeenCalledExactlyOnceWith('Copy');
        expect(duplicate.visible).toBe(false);
    });

    it('does not blindly resubmit or dismiss an unknown write outcome without a job identity', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        transport.startSampleDuplication.mockRejectedValueOnce(new Error('Connection lost before acknowledgement'));
        await duplicate.submit();
        expect(duplicate.phase).toBe('unconfirmed');
        expect(duplicate.canCheck).toBe(false);
        expect(duplicate.locked).toBe(true);
        expect(duplicate.canSubmit).toBe(false);
        await duplicate.submit();
        await duplicate.recover();
        duplicate.close();
        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(1);
        expect(transport.waitForJob).not.toHaveBeenCalled();
        expect(duplicate.visible).toBe(true);
        expect(oncreated).not.toHaveBeenCalled();
    });

    it('allows an explicit retry only after the acknowledged job confirms failure', async () => {
        const { duplicate, transport, oncreated } = setup();
        await duplicate.open(1, 'Source', ['Source'], oncreated);
        duplicate.name = 'Copy';
        transport.waitForJob.mockResolvedValueOnce({ jobId: 7, status: 'failed', error: 'Destination name exists' });
        await duplicate.submit();
        expect(duplicate.phase).toBe('editable');
        expect(duplicate.message).toContain('Destination name exists');
        expect(oncreated).not.toHaveBeenCalled();
        duplicate.name = 'Another copy';
        await duplicate.submit();
        expect(transport.startSampleDuplication).toHaveBeenCalledTimes(2);
        expect(oncreated).toHaveBeenCalledExactlyOnceWith('Another copy');
    });
});
