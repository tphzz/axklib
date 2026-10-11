import { describe, expect, it, vi } from 'vitest';
import { OpenSourceWorkflow } from './openSource.svelte';
import type { DeviceService, DeviceSession } from '../../lib/httpDevices';
import type { ImageSessionPreparation } from '../image-session/actions';
import type { ImageLocation } from '../../lib/storageLocations';
import type { OpenedImage } from '../../lib/transport';

const card = { id: 'card', model: 'Reader', serial: 'serial', sizeBytes: 32_000_000_000, sectorSize: 512 };
const target = {
    id: '2',
    label: 'SCSI ID 2',
    offsetBytes: 1_024_000,
    sizeBytes: 2_048_000,
    sectorSize: 2048,
    readOnly: true,
    notices: [],
    scsi: {
        id: 2,
        deviceType: 'OPTICAL' as const,
        vendor: 'codesrc',
        product: 'CD-ROM',
        revision: '6.0',
        serial: 'target',
    },
};
const session: DeviceSession = {
    sessionId: 'preview',
    candidate: card,
    targets: [target],
    writesSupported: true,
    writesAuthorized: false,
    invalidated: false,
};
function fixture() {
    const service = {
        available: vi.fn(async () => true),
        list: vi.fn(async () => ({ devices: [card], writesSupported: true })),
        connect: vi.fn(async () => session),
        status: vi.fn(async () => session),
        close: vi.fn(async () => {}),
        authorize: vi.fn(async () => {}),
        inspectXml: vi.fn(),
        recover: vi.fn(),
        operations: vi.fn(),
    } satisfies DeviceService;
    const open = vi.fn(async () => true);
    return { service, open, workflow: new OpenSourceWorkflow(service, open) };
}
async function initialize(workflow: OpenSourceWorkflow) {
    await workflow.initialize();
    await workflow.activate('SCSI2SDv6');
}
describe('SCSI2SDv6 opening', () => {
    it('reads only the selected carrier automatically and requires a target selection', async () => {
        const { workflow, service, open } = fixture();
        await initialize(workflow);
        expect(service.connect).not.toHaveBeenCalled();
        expect(workflow.visibleCandidates).toEqual([card]);
        await workflow.selectV6Device(card.id);
        expect(service.connect).toHaveBeenCalledWith(card.id, 'SCSI2SD_V6');
        expect(workflow.v6Targets).toEqual([target]);
        expect(workflow.canOpen).toBe(false);
        workflow.v6TargetId = target.id;
        expect(workflow.access).toBe('read-only');
        expect(workflow.canOpen).toBe(true);
        expect(await workflow.open()).toBe(true);
        expect(service.connect).toHaveBeenCalledTimes(1);
        expect(service.status).toHaveBeenCalledWith('preview');
        expect(service.authorize).not.toHaveBeenCalled();
        expect(service.close).not.toHaveBeenCalled();
        expect(open).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { sessionId: 'preview', targetId: '2' } }),
            undefined,
        );
    });
    it('keeps a matching refreshed selection without reopening or losing acknowledgement', async () => {
        const { workflow, service } = fixture();
        service.connect.mockResolvedValue({ ...session, targets: [{ ...target, sectorSize: 512, readOnly: false }] });
        service.status.mockImplementation(async () => ({
            ...session,
            targets: [{ ...target, sectorSize: 512, readOnly: false }],
        }));
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        workflow.v6TargetId = target.id;
        workflow.acknowledged = true;
        await workflow.refresh();
        expect(workflow.v6CandidateId).toBe(card.id);
        expect(workflow.v6TargetId).toBe(target.id);
        expect(workflow.acknowledged).toBe(true);
        expect(service.connect).toHaveBeenCalledTimes(1);
        await workflow.cancel();
        expect(service.close).toHaveBeenCalledWith('preview');
    });
    it('does not display targets for an unrelated card or permission denial', async () => {
        const { workflow, service } = fixture();
        service.connect.mockRejectedValue(new Error('No SCSI2SDv6 configuration found'));
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        expect(workflow.status).toContain('No SCSI2SDv6 configuration');
        expect(workflow.v6Targets).toEqual([]);
        expect(workflow.canOpen).toBe(false);
        expect(service.authorize).not.toHaveBeenCalled();
    });
    it('closes a stale asynchronous result before reading a replacement carrier', async () => {
        const { workflow, service } = fixture();
        const second = { ...card, id: 'second' };
        service.list.mockResolvedValue({ devices: [card, second], writesSupported: true });
        let complete!: (value: DeviceSession) => void;
        service.connect.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        await initialize(workflow);
        const firstRead = workflow.selectV6Device(card.id);
        await vi.waitFor(() => expect(service.connect).toHaveBeenCalledTimes(1));
        const secondRead = workflow.selectV6Device(second.id);
        expect(workflow.v6Targets).toEqual([]);
        complete(session);
        await firstRead;
        await secondRead;
        expect(service.close).toHaveBeenCalledWith('preview');
        expect(service.connect).toHaveBeenLastCalledWith(second.id, 'SCSI2SD_V6');
        expect(workflow.v6CandidateId).toBe(second.id);
        await workflow.cancel();
    });
    it('releases the preview when switching tabs', async () => {
        const { workflow, service } = fixture();
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        await workflow.activate('Image');
        expect(workflow.tab).toBe('Image');
        expect(workflow.v6Targets).toEqual([]);
        expect(service.close).toHaveBeenCalledWith('preview');
    });
    it('rejects an invalidated preview before opening an image', async () => {
        const { workflow, service, open } = fixture();
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        workflow.v6TargetId = target.id;
        service.status.mockResolvedValue({ ...session, invalidated: true });
        expect(await workflow.open()).toBe(false);
        expect(open).not.toHaveBeenCalled();
        expect(service.close).toHaveBeenCalledWith('preview');
        expect(workflow.v6Targets).toEqual([]);
    });
    it('authorizes only the explicitly selected disk during Open, never during preview', async () => {
        const { service } = fixture();
        const disk = { ...target, id: '0', readOnly: false, sectorSize: 512 };
        service.connect.mockResolvedValue({ ...session, targets: [disk, target] });
        service.status.mockResolvedValue({ ...session, targets: [disk, target] });
        const open = vi.fn(async (_location: ImageLocation, prepare?: ImageSessionPreparation) => {
            await prepare?.({ format: 'sfs' } as OpenedImage);
            return true;
        });
        const workflow = new OpenSourceWorkflow(service, open);
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        expect(service.authorize).not.toHaveBeenCalled();
        workflow.v6TargetId = '0';
        expect(await workflow.open()).toBe(false);
        workflow.acknowledged = true;
        expect(await workflow.open()).toBe(true);
        expect(service.authorize).toHaveBeenCalledWith('preview', '0');
    });
    it('clears a vanished carrier and its targets without opening a replacement', async () => {
        const { workflow, service } = fixture();
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        workflow.v6TargetId = target.id;
        service.list.mockResolvedValue({ devices: [{ ...card, id: 'replacement' }], writesSupported: true });
        await workflow.refresh();
        expect(workflow.v6CandidateId).toBe('');
        expect(workflow.v6TargetId).toBe('');
        expect(workflow.v6Targets).toEqual([]);
        expect(workflow.canOpen).toBe(false);
        expect(service.connect).toHaveBeenCalledTimes(1);
        expect(service.close).toHaveBeenCalledWith('preview');
    });
    it('blocks opening after a failed status refresh and permits an explicit refresh retry', async () => {
        const { workflow, service } = fixture();
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        workflow.v6TargetId = target.id;
        service.status.mockRejectedValueOnce(new Error('Device disconnected'));
        await workflow.refresh();
        expect(workflow.status).toContain('Device disconnected');
        expect(workflow.v6Targets).toEqual([]);
        expect(workflow.canOpen).toBe(false);
        await workflow.refresh();
        expect(service.connect).toHaveBeenCalledTimes(2);
        expect(workflow.v6TargetId).toBe('');
        await workflow.cancel();
    });
    it('retains ownership on cleanup failure and prevents opening or switching tabs until cleanup succeeds', async () => {
        const { workflow, service } = fixture();
        await initialize(workflow);
        await workflow.selectV6Device(card.id);
        service.close.mockRejectedValue(new Error('Connection is busy'));
        await workflow.activate('Image');
        expect(workflow.tab).toBe('SCSI2SDv6');
        expect(workflow.connection?.sessionId).toBe('preview');
        expect(workflow.v6Targets).toEqual([]);
        expect(workflow.canOpen).toBe(false);
        expect(await workflow.cancel()).toBe(false);
        service.close.mockResolvedValue();
        expect(await workflow.cancel()).toBe(true);
        expect(workflow.connection).toBeNull();
    });
    it('waits for a late permission response and releases it on cancellation', async () => {
        const { workflow, service } = fixture();
        let complete!: (value: DeviceSession) => void;
        service.connect.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        await initialize(workflow);
        const reading = workflow.selectV6Device(card.id);
        await vi.waitFor(() => expect(service.connect).toHaveBeenCalledOnce());
        const cancelled = workflow.cancel();
        complete(session);
        await reading;
        expect(await cancelled).toBe(true);
        expect(workflow.v6Targets).toEqual([]);
        expect(service.close).toHaveBeenCalledWith('preview');
    });
});
