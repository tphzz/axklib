import { describe, expect, it, vi } from 'vitest';
import { CreateWorkflow } from './workflow.svelte';
import { deviceFormatInspection, deviceFormatSession } from '../../test/deviceFormatFixture';
import type { DeviceService } from '../../lib/httpDevices';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import type { ImageLocation } from '../../lib/storageLocations';
import type { JobState } from '../../lib/transport';
import { AxklibApiError } from '../../lib/httpErrors';

function fixture() {
    const session = deviceFormatSession();
    const inspection = deviceFormatInspection(session);
    const formatting = {
        available: vi.fn(async () => true),
        inspect: vi.fn(async () => inspection),
        plan: vi.fn(async () => ({
            planToken: 'reviewed-plan',
            inspection,
            partitionCount: 8,
            expiresInSeconds: 900 as const,
        })),
        start: vi.fn(async (): Promise<JobState> => ({
            jobId: 1,
            serverJobId: 'format-job',
            kind: 'devices.sfs.format',
            status: 'queued',
        })),
        wait: vi.fn(async (): Promise<JobState> => ({
            jobId: 1,
            kind: 'devices.sfs.format',
            status: 'completed',
            result: {
                operationId: 'operation',
                sessionId: session.sessionId,
                targetId: 'disk',
                sizeBytes: 8589934592,
                partitionCount: 8,
            },
        })),
        cancel: vi.fn(async () => {}),
        status: vi.fn(),
        resolve: vi.fn(async () => ({
            operationId: 'operation',
            kind: 'FORMAT' as const,
            state: 'NO_WRITE' as 'NO_WRITE' | 'COMMITTED' | 'RECOVERY_REQUIRED',
            model: session.candidate.model,
            serial: session.candidate.serial,
            targetLabel: 'Disk',
            targetSize: session.candidate.sizeBytes,
        })),
    };
    const service = {
        formatting,
        available: vi.fn(async () => true),
        list: vi.fn(async () => ({ devices: [session.candidate], writesSupported: true })),
        connect: vi.fn(async () => session),
        status: vi.fn(async () => session),
        close: vi.fn(async () => {}),
        authorize: vi.fn(),
        inspectXml: vi.fn(),
        recover: vi.fn(),
        operations: vi.fn(),
    } satisfies DeviceService;
    const images = {
        confirmEditorLeave: vi.fn(async () => true),
        location: null as ImageLocation | null,
        open: vi.fn(async ([location]: ImageLocation[]) => {
            images.location = location!;
        }),
        setStatus: vi.fn(),
    };
    const workflow = new CreateWorkflow(service, images as unknown as ImageSessionWorkflow);
    return { workflow, service, formatting, inspection, images, session };
}
async function prepared(f: ReturnType<typeof fixture>) {
    await f.workflow.show();
    await f.workflow.activate('Device');
    await f.workflow.select('card');
    await f.workflow.review();
    f.workflow.confirmationText = f.inspection.confirmationText;
    f.workflow.dataLossAcknowledged = true;
    f.workflow.exclusiveUseAcknowledged = true;
}

describe('Create device ownership', () => {
    it('defaults to Image without listing, connecting, authorizing or opening a directory picker', async () => {
        const f = fixture();
        await f.workflow.show();
        expect(f.workflow.selection.tab).toBe('Image');
        expect(f.service.list).not.toHaveBeenCalled();
        expect(f.service.connect).not.toHaveBeenCalled();
        expect(f.service.authorize).not.toHaveBeenCalled();
        expect(f.formatting.start).not.toHaveBeenCalled();
    });
    it('requires validated XML before enumerating carriers in the XML destination', async () => {
        const f = fixture();
        await f.workflow.show();
        await f.workflow.activate('SCSI2SD XML');
        expect(f.service.list).not.toHaveBeenCalled();
        await f.workflow.selection.refresh();
        expect(f.service.list).not.toHaveBeenCalled();
        f.service.inspectXml.mockRejectedValueOnce(new Error('Invalid configuration XML'));
        await f.workflow.selection.loadXml({ name: 'invalid.xml', size: 8, text: async () => '<broken>' });
        expect(f.service.inspectXml).toHaveBeenCalledOnce();
        expect(f.service.list).not.toHaveBeenCalled();
        expect(f.service.connect).not.toHaveBeenCalled();
    });
    it('uses the selected target capacity, requires exact consent and never calls ordinary authorization', async () => {
        const f = fixture();
        await prepared(f);
        expect(f.workflow.partitionCount).toBe(8);
        expect(f.images.confirmEditorLeave).toHaveBeenCalledOnce();
        f.workflow.confirmationText += ' ';
        await f.workflow.format();
        expect(f.formatting.start).not.toHaveBeenCalled();
        f.workflow.confirmationText = f.inspection.confirmationText;
        await f.workflow.format();
        expect(f.service.authorize).not.toHaveBeenCalled();
        expect(f.formatting.start).toHaveBeenCalledOnce();
        expect(f.workflow.visible).toBe(false);
        expect(f.images.location?.kind).toBe('device');
    });
    it('invalidates all review consent after cancellation and a partition change', async () => {
        const f = fixture();
        await prepared(f);
        f.workflow.cancelConfirmation();
        f.workflow.setPartitions(7);
        expect(f.workflow.plan).toBeNull();
        expect(f.workflow.confirmationText).toBe('');
        expect(f.workflow.dataLossAcknowledged).toBe(false);
        expect(f.workflow.canReview).toBe(false);
        expect(f.workflow.canFormat).toBe(false);
    });
    it('blocks planning for mounted or incomplete inspection results', async () => {
        const f = fixture();
        f.inspection.blockedReasons = ['Unmount using the operating system, then inspect again.'];
        await prepared(f);
        expect(f.workflow.canReview).toBe(false);
        expect(f.formatting.plan).not.toHaveBeenCalled();
        expect(f.workflow.canFormat).toBe(false);
    });
    it('keeps queued work owned through cancellation and does not release the target early', async () => {
        const f = fixture();
        let finish!: (job: JobState) => void;
        f.formatting.wait.mockImplementation(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        await prepared(f);
        const writing = f.workflow.format();
        await vi.waitFor(() => expect(f.formatting.wait).toHaveBeenCalledOnce());
        expect(f.workflow.locked).toBe(true);
        await f.workflow.cancel();
        await f.workflow.activate('Image');
        expect(f.workflow.visible).toBe(true);
        expect(f.workflow.selection.tab).toBe('Device');
        expect(f.service.close).not.toHaveBeenCalled();
        finish({ jobId: 1, kind: 'devices.sfs.format', status: 'cancelled' });
        await writing;
        expect(f.workflow.locked).toBe(false);
        expect(f.service.close).toHaveBeenCalledOnce();
    });
    it('reconciles a lost acknowledgement with durable status, never another format submission', async () => {
        const f = fixture();
        f.formatting.start.mockRejectedValueOnce(new Error('Lost acknowledgement'));
        f.formatting.resolve.mockResolvedValueOnce({ ...(await f.formatting.resolve()), state: 'COMMITTED' });
        await prepared(f);
        await f.workflow.format();
        expect(f.workflow.phase).toBe('unconfirmed');
        expect(f.workflow.operationId).not.toBe('');
        await f.workflow.cancel();
        expect(f.workflow.visible).toBe(true);
        await f.workflow.checkStatus();
        expect(f.formatting.start).toHaveBeenCalledOnce();
        expect(f.images.open).toHaveBeenCalledOnce();
        expect(f.workflow.phase).toBe('complete');
    });
    it('keeps ownership when an acknowledged completed job has an HTTP refusal during status retrieval', async () => {
        const f = fixture();
        f.formatting.start.mockResolvedValueOnce({
            jobId: 1,
            kind: 'devices.sfs.format',
            status: 'completed',
        });
        f.formatting.status.mockRejectedValueOnce(new AxklibApiError('forbidden', 'Status unavailable', 403));
        await prepared(f);
        await f.workflow.format();
        expect(f.workflow.phase).toBe('unconfirmed');
        expect(f.workflow.locked).toBe(true);
        expect(f.workflow.job?.jobId).toBe(1);
        expect(f.workflow.canReview).toBe(false);
        await f.workflow.cancel();
        expect(f.service.close).not.toHaveBeenCalled();
        expect(f.formatting.start).toHaveBeenCalledOnce();
    });
    it('does not rewrite when opening the confirmed new filesystem fails', async () => {
        const f = fixture();
        f.images.open.mockRejectedValueOnce(new Error('Read connection interrupted'));
        await prepared(f);
        await f.workflow.format();
        expect(f.workflow.phase).toBe('complete');
        expect(f.workflow.visible).toBe(true);
        expect(f.workflow.canReview).toBe(false);
        await f.workflow.activate('Image');
        expect(f.workflow.selection.tab).toBe('Device');
        await f.workflow.openFormatted();
        expect(f.formatting.start).toHaveBeenCalledOnce();
        expect(f.images.open).toHaveBeenCalledTimes(2);
    });
    it('reconnects read-only after a failed reopen has already closed the original native session', async () => {
        const f = fixture();
        f.images.open.mockImplementationOnce(async () => {
            f.service.close.mockRejectedValueOnce(new AxklibApiError('device_session_missing', 'Closed', 404));
            throw new Error('Open failed after closing the read connection');
        });
        await prepared(f);
        await f.workflow.format();
        expect(f.workflow.phase).toBe('complete');
        f.service.connect.mockResolvedValueOnce({ ...f.session, sessionId: 'reconnected-read-only' });
        await f.workflow.openFormatted();
        expect(f.service.connect).toHaveBeenCalledTimes(2);
        expect(f.service.connect).toHaveBeenLastCalledWith('card', 'RAW', '');
        expect(f.images.location?.kind === 'device' && f.images.location.reference.sessionId).toBe(
            'reconnected-read-only',
        );
        expect(f.service.authorize).not.toHaveBeenCalled();
        expect(f.formatting.start).toHaveBeenCalledOnce();
    });
    it('does not reopen a different carrier with the same display name after a confirmed format', async () => {
        const f = fixture();
        f.images.open.mockRejectedValueOnce(new Error('Disconnected after commit'));
        await prepared(f);
        await f.workflow.format();
        f.service.list.mockResolvedValueOnce({
            devices: [{ ...f.session.candidate, id: 'different-card' }],
            writesSupported: true,
        });
        await f.workflow.openFormatted();
        expect(f.workflow.visible).toBe(true);
        expect(f.workflow.phase).toBe('complete');
        expect(f.images.open).toHaveBeenCalledOnce();
        expect(f.service.connect).toHaveBeenCalledOnce();
        expect(f.formatting.start).toHaveBeenCalledOnce();
        expect(f.workflow.canReview).toBe(false);
    });
    it('returns to review on definite refusal and retains recovery ownership on durable failure', async () => {
        const f = fixture();
        f.formatting.start.mockRejectedValueOnce(new AxklibApiError('device_busy', 'Mounted', 422));
        await prepared(f);
        await f.workflow.format();
        expect(f.workflow.phase).toBe('editing');
        await f.workflow.review();
        f.workflow.confirmationText = f.inspection.confirmationText;
        f.workflow.dataLossAcknowledged = f.workflow.exclusiveUseAcknowledged = true;
        f.formatting.wait.mockResolvedValueOnce({ jobId: 1, kind: 'devices.sfs.format', status: 'failed' });
        f.formatting.resolve.mockResolvedValueOnce({ ...(await f.formatting.resolve()), state: 'RECOVERY_REQUIRED' });
        await f.workflow.format();
        expect(f.workflow.phase).toBe('recovery');
        expect(f.workflow.locked).toBe(true);
        expect(f.service.close).toHaveBeenCalledOnce();
    });
});
