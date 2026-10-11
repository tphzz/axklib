import { describe, expect, it, vi } from 'vitest';
import { OpenSourceWorkflow } from './openSource.svelte';
import type { ImageSessionPreparation } from '../image-session/actions';
import type { DeviceService, DeviceSession } from '../../lib/httpDevices';
import type { ImageLocation } from '../../lib/storageLocations';
import type { OpenedImage } from '../../lib/transport';
import { AxklibApiError } from '../../lib/httpErrors';

const candidate = { id: 'usb', model: 'Card', serial: 'test', sizeBytes: 1024, sectorSize: 512 };
const target = {
    id: 'disk',
    label: 'Disk',
    sizeBytes: 1024,
    offsetBytes: 0,
    scsi: null,
    sectorSize: 512,
    readOnly: false,
    notices: [],
};
const connection: DeviceSession = {
    sessionId: 'owned',
    candidate,
    targets: [target],
    writesSupported: true,
    writesAuthorized: false,
    invalidated: false,
};
const image = { sessionId: 1, format: 'sfs' } as OpenedImage;

function fixture() {
    const service = {
        available: vi.fn(async () => true),
        inspectXml: vi.fn(async () => ({ targets: [{ ...target, id: '0', enabled: true, eligible: true }] })),
        list: vi.fn(async () => ({ devices: [candidate], writesSupported: true })),
        connect: vi.fn(async () => connection),
        close: vi.fn(async () => {}),
        status: vi.fn(),
        authorize: vi.fn(async () => {}),
        recover: vi.fn(),
        operations: vi.fn(),
    } satisfies DeviceService;
    const cleanup = vi.fn(async () => {});
    const openImage = vi.fn(async (_location: ImageLocation, prepare?: ImageSessionPreparation) => {
        await prepare?.(image);
        return true;
    });
    return { service, cleanup, openImage, workflow: new OpenSourceWorkflow(service, openImage, cleanup) };
}

async function selectDevice(workflow: OpenSourceWorkflow) {
    await workflow.initialize();
    await workflow.activate('Device');
    workflow.candidateId = 'usb';
}

describe('device access during Open', () => {
    it('defaults to write access but requires exclusive-use acknowledgement before connecting', async () => {
        const { workflow, service } = fixture();
        await selectDevice(workflow);
        expect(workflow.access).toBe('read-write');
        expect(workflow.acknowledged).toBe(false);
        expect(workflow.canOpen).toBe(false);
        expect(await workflow.open()).toBe(false);
        expect(service.connect).not.toHaveBeenCalled();
        workflow.acknowledged = true;
        expect(await workflow.open()).toBe(true);
        expect(service.authorize).toHaveBeenCalledWith('owned', 'disk');
        expect(workflow.connection).toBeNull();
        await workflow.dispose();
        expect(service.close).not.toHaveBeenCalled();
    });

    it('keeps read-only opening available without acknowledging or authorizing', async () => {
        const { workflow, service, openImage } = fixture();
        await selectDevice(workflow);
        workflow.access = 'read-only';
        expect(workflow.canOpen).toBe(true);
        expect(await workflow.open()).toBe(true);
        expect(openImage.mock.calls[0]?.[1]).toBeUndefined();
        expect(service.authorize).not.toHaveBeenCalled();
    });

    it('forces read-only access on unsupported platforms and optical targets', async () => {
        const { workflow, service } = fixture();
        service.list.mockResolvedValue({ devices: [candidate], writesSupported: false });
        await selectDevice(workflow);
        expect(workflow.access).toBe('read-only');
        expect(workflow.writeUnavailableReason).toMatch(/platform/);
        expect(workflow.canOpen).toBe(true);
        service.list.mockResolvedValue({ devices: [{ ...candidate, sectorSize: 2048 }], writesSupported: true });
        await workflow.refresh();
        expect(workflow.access).toBe('read-only');
        expect(workflow.writeUnavailableReason).toMatch(/read-only/);
    });

    it('resets acknowledgement on identity and access changes, but preserves it for a matching refresh', async () => {
        const { workflow } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        await workflow.refresh();
        expect(workflow.acknowledged).toBe(true);
        workflow.candidateId = '';
        workflow.candidateId = 'usb';
        expect(workflow.acknowledged).toBe(false);
        workflow.acknowledged = true;
        workflow.access = 'read-only';
        workflow.access = 'read-write';
        expect(workflow.acknowledged).toBe(false);
        workflow.acknowledged = true;
        await workflow.activate('Image');
        await workflow.activate('Device');
        expect(workflow.acknowledged).toBe(false);
    });

    it('authorizes only the explicitly selected XML target and resets acknowledgement on configuration changes', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('SCSI2SD XML');
        await workflow.loadXml({ name: 'card.xml', size: 1, text: async () => 'xml' });
        workflow.xmlCandidateId = 'usb';
        workflow.xmlTargetId = '0';
        workflow.acknowledged = true;
        workflow.xmlTargetId = '';
        workflow.xmlTargetId = '0';
        expect(workflow.acknowledged).toBe(false);
        workflow.acknowledged = true;
        await workflow.loadXml({ name: 'card.xml', size: 1, text: async () => 'xml' });
        expect(workflow.acknowledged).toBe(false);
        workflow.xmlCandidateId = 'usb';
        workflow.xmlTargetId = '0';
        workflow.acknowledged = true;
        service.connect.mockResolvedValue({ ...connection, targets: [{ ...target, id: '0' }] });
        expect(await workflow.open()).toBe(true);
        expect(service.authorize).toHaveBeenCalledWith('owned', '0');
    });

    it('does not silently downgrade when connected layout checks disallow writing', async () => {
        const { workflow, service, openImage } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        service.connect.mockResolvedValue({ ...connection, targets: [{ ...target, readOnly: true }] });
        expect(await workflow.open()).toBe(false);
        expect(workflow.status).toMatch(/read.only/i);
        expect(workflow.status).toContain('Read only');
        expect(openImage).not.toHaveBeenCalled();
        expect(service.authorize).not.toHaveBeenCalled();
        expect(service.close).toHaveBeenCalledWith('owned');
    });

    it('rejects non-SFS media before authorization', async () => {
        const { workflow, service, openImage } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        openImage.mockImplementation(async (_location, prepare) => {
            await prepare?.({ ...image, format: 'iso9660' });
            return true;
        });
        expect(await workflow.open()).toBe(false);
        expect(service.authorize).not.toHaveBeenCalled();
        expect(service.close).toHaveBeenCalledWith('owned');
    });

    it('retains a permission error without adopting or silently reopening the device', async () => {
        const { workflow, service, cleanup } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        service.authorize.mockRejectedValue(new Error('Administrator permission was declined'));
        expect(await workflow.open()).toBe(false);
        expect(workflow.status).toBe('Administrator permission was declined');
        expect(workflow.busy).toBe(false);
        expect(service.connect).toHaveBeenCalledOnce();
        expect(cleanup).toHaveBeenCalledOnce();
        expect(service.close).toHaveBeenCalledWith('owned');
        workflow.access = 'read-only';
        expect(await workflow.open()).toBe(true);
        expect(service.authorize).toHaveBeenCalledOnce();
    });

    it('keeps authorization busy and blocks dismissal until the prompt finishes', async () => {
        const { workflow, service } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        let finish!: () => void;
        service.authorize.mockImplementation(() => new Promise<void>((resolve) => (finish = resolve)));
        const opening = workflow.open();
        await vi.waitFor(() => expect(service.authorize).toHaveBeenCalledOnce());
        expect(workflow.busy).toBe(true);
        expect(await workflow.cancel()).toBe(false);
        expect(await workflow.open()).toBe(false);
        finish();
        expect(await opening).toBe(true);
    });

    it('retains failed candidate cleanup and retries only cleanup on cancellation', async () => {
        const { workflow, service, cleanup } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        service.authorize.mockRejectedValue(new Error('Denied'));
        cleanup.mockRejectedValue(new Error('Cannot close staged image'));
        expect(await workflow.open()).toBe(false);
        expect(workflow.status).toContain('Connection cleanup failed');
        expect(workflow.connection?.sessionId).toBe('owned');
        expect(service.close).not.toHaveBeenCalled();
        expect(await workflow.cancel()).toBe(false);
        cleanup.mockResolvedValue(undefined);
        expect(await workflow.cancel()).toBe(true);
        expect(service.authorize).toHaveBeenCalledOnce();
    });

    it('accepts a device connection already released by staged-image cleanup', async () => {
        const { workflow, service, openImage } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        openImage.mockRejectedValue(new Error('Permission declined'));
        service.close.mockRejectedValue(new AxklibApiError('device_session_missing', 'Already released', 404));
        expect(await workflow.open()).toBe(false);
        expect(workflow.connection).toBeNull();
        expect(workflow.status).toBe('Permission declined');
    });

    it('does not authorize when opening is cancelled before candidate preparation', async () => {
        const { workflow, service, openImage } = fixture();
        await selectDevice(workflow);
        workflow.acknowledged = true;
        openImage.mockResolvedValue(false);
        expect(await workflow.open()).toBe(false);
        expect(service.authorize).not.toHaveBeenCalled();
        expect(service.close).toHaveBeenCalledWith('owned');
    });
});
