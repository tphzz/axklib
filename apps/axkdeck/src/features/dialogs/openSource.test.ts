import { describe, expect, it, vi } from 'vitest';
import { OpenSourceWorkflow } from './openSource.svelte';
import type { DeviceService, DeviceSession, DeviceXmlInspection } from '../../lib/httpDevices';

const candidate = { id: 'usb', model: 'Card', serial: 'test', sizeBytes: 1024, sectorSize: 512 };
const target: DeviceSession['targets'][number] = {
    id: '0',
    label: 'SCSI ID 0',
    sizeBytes: 1024,
    offsetBytes: 0,
    scsi: null,
    sectorSize: 512,
    readOnly: true,
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
function fixture() {
    const service = {
        available: vi.fn(async () => true),
        inspectXml: vi.fn(async (): Promise<DeviceXmlInspection> => ({
            targets: [{ ...target, enabled: true, eligible: true }],
        })),
        list: vi.fn(async () => ({ devices: [candidate], writesSupported: true })),
        connect: vi.fn(async () => ({ ...connection, targets: [{ ...target, id: 'disk' }] })),
        close: vi.fn(async () => {}),
        status: vi.fn(),
        authorize: vi.fn(),
        recover: vi.fn(),
        operations: vi.fn(),
    } satisfies DeviceService;
    const openImage = vi.fn(async () => true);
    const workflow = new OpenSourceWorkflow(service, openImage);
    workflow.access = 'read-only';
    return { service, openImage, workflow };
}
describe('unified Open ownership', () => {
    it('retains both device selections and the XML target across refresh and reordering', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        await workflow.activate('SCSI2SD XML');
        await workflow.loadXml({ name: 'card.xml', size: 1, text: async () => '<SCSI2SD/>' });
        workflow.xmlCandidateId = 'usb';
        workflow.xmlTargetId = '0';
        service.list.mockResolvedValue({ devices: [{ ...candidate, id: 'other' }, candidate], writesSupported: true });
        await workflow.refresh();
        expect(workflow.candidateId).toBe('usb');
        expect(workflow.xmlCandidateId).toBe('usb');
        expect(workflow.xmlTargetId).toBe('0');
        expect(workflow.canOpen).toBe(true);
        await workflow.activate('Device');
        expect(workflow.selectedCandidate?.id).toBe('usb');
    });
    it('clears vanished selections without selecting an identically named replacement', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        workflow.xmlCandidateId = 'usb';
        service.list.mockResolvedValue({ devices: [{ ...candidate, id: 'replacement' }], writesSupported: true });
        await workflow.refresh();
        expect(workflow.candidateId).toBe('');
        expect(workflow.xmlCandidateId).toBe('');
        expect(workflow.canOpen).toBe(false);
    });
    it('reconciles each tab against its own eligibility filter', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        workflow.xmlCandidateId = 'usb';
        service.list.mockResolvedValue({ devices: [{ ...candidate, sectorSize: 2048 }], writesSupported: true });
        await workflow.refresh();
        expect(workflow.candidateId).toBe('usb');
        expect(workflow.xmlCandidateId).toBe('');
    });
    it('keeps selection visible on failed refresh but blocks opening until refresh succeeds', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        service.list.mockRejectedValueOnce(new Error('Enumeration failed'));
        await workflow.refresh();
        expect(workflow.candidateId).toBe('usb');
        expect(workflow.selectedCandidate).toEqual(candidate);
        expect(workflow.status).toBe('Enumeration failed');
        expect(workflow.canOpen).toBe(false);
        await workflow.open();
        expect(service.connect).not.toHaveBeenCalled();
        await workflow.refresh();
        expect(workflow.canOpen).toBe(true);
    });
    it('keeps selections while a refresh is pending and ignores superseded results', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        let complete!: (value: Awaited<ReturnType<DeviceService['list']>>) => void;
        service.list.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        const refreshing = workflow.refresh();
        expect(workflow.candidateId).toBe('usb');
        expect(workflow.canOpen).toBe(false);
        await workflow.refresh();
        complete({ devices: [], writesSupported: true });
        await refreshing;
        expect(workflow.selectedCandidate?.id).toBe('usb');
        expect(workflow.canOpen).toBe(true);
    });
    it('does not enumerate on Image and never selects a device automatically', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        expect(workflow.tab).toBe('Image');
        expect(service.list).not.toHaveBeenCalled();
        await workflow.activate('Device');
        expect(service.list).toHaveBeenCalledOnce();
        expect(workflow.candidateId).toBe('');
        await workflow.open();
        expect(service.connect).not.toHaveBeenCalled();
    });
    it('connects and opens read-only without authorizing, then transfers ownership', async () => {
        const { workflow, service, openImage } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        expect(await workflow.open()).toBe(true);
        expect(service.connect).toHaveBeenCalledWith('usb', 'RAW', undefined);
        expect(openImage).toHaveBeenCalledWith(
            expect.objectContaining({ reference: { sessionId: 'owned', targetId: 'disk' } }),
            undefined,
        );
        await workflow.dispose();
        expect(service.close).not.toHaveBeenCalled();
        expect(service.authorize).not.toHaveBeenCalled();
    });
    it('closes untransferred connections when image opening fails or is cancelled', async () => {
        const { workflow, service, openImage } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        openImage.mockResolvedValue(false);
        expect(await workflow.open()).toBe(false);
        expect(service.close).toHaveBeenCalledWith('owned');
        expect(workflow.busy).toBe(false);
    });
    it('validates XML natively before enumeration and requires an explicit target', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('SCSI2SD XML');
        expect(service.list).not.toHaveBeenCalled();
        await workflow.loadXml({ name: 'card.xml', size: 1, text: async () => '<SCSI2SD/>' });
        expect(service.inspectXml).toHaveBeenCalledWith('<SCSI2SD/>');
        expect(service.list).toHaveBeenCalledOnce();
        expect(workflow.xmlTargetId).toBe('');
        workflow.xmlCandidateId = 'usb';
        workflow.xmlTargetId = '0';
        service.connect.mockResolvedValue(connection);
        expect(await workflow.open()).toBe(true);
        expect(service.connect).toHaveBeenCalledWith('usb', 'SCSI2SD_XML', '<SCSI2SD/>');
    });
    it('ignores a stale XML result and a device list arriving after cancellation', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('SCSI2SD XML');
        let complete!: (value: Awaited<ReturnType<DeviceService['inspectXml']>>) => void;
        service.inspectXml.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        const first = workflow.loadXml({ name: 'old.xml', size: 1, text: async () => 'old' });
        await vi.waitFor(() => expect(service.inspectXml).toHaveBeenCalled());
        await workflow.loadXml({ name: 'new.xml', size: 1, text: async () => 'new' });
        complete({ targets: [] });
        await first;
        expect(workflow.xmlName).toBe('new.xml');
        expect(workflow.xmlTargets).toHaveLength(1);
        await workflow.dispose();
        await workflow.refresh();
        expect(service.list).toHaveBeenCalledOnce();
    });
    it('cleans a connection that arrives after disposal without opening it', async () => {
        const { workflow, service, openImage } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        let complete!: (value: DeviceSession) => void;
        service.connect.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        const opening = workflow.open();
        await workflow.dispose();
        complete(connection);
        await opening;
        expect(openImage).not.toHaveBeenCalled();
        expect(service.close).toHaveBeenCalledWith('owned');
    });
    it('retains a failed cleanup connection and retries only cleanup on cancellation', async () => {
        const { workflow, service, openImage } = fixture();
        await workflow.initialize();
        await workflow.activate('Device');
        workflow.candidateId = 'usb';
        openImage.mockResolvedValue(false);
        service.close.mockRejectedValue(new Error('Offline'));
        expect(await workflow.open()).toBe(false);
        expect(workflow.connection?.sessionId).toBe('owned');
        expect(workflow.status).toContain('Connection cleanup failed');
        expect(workflow.canOpen).toBe(false);
        expect(await workflow.cancel()).toBe(false);
        await workflow.activate('Image');
        expect(workflow.tab).toBe('Device');
        service.close.mockResolvedValue(undefined);
        expect(await workflow.cancel()).toBe(true);
        expect(workflow.connection).toBeNull();
        expect(service.connect).toHaveBeenCalledOnce();
    });
    it('ignores enumeration completing after the dialog has been disposed', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        let complete!: (value: Awaited<ReturnType<DeviceService['list']>>) => void;
        service.list.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    complete = resolve;
                }),
        );
        const listing = workflow.activate('Device');
        await workflow.dispose();
        complete({ devices: [candidate], writesSupported: true });
        await listing;
        expect(workflow.candidates).toEqual([]);
        expect(service.connect).not.toHaveBeenCalled();
    });
    it('rejects invalid or oversized XML before enumeration', async () => {
        const { workflow, service } = fixture();
        await workflow.initialize();
        await workflow.activate('SCSI2SD XML');
        service.inspectXml.mockRejectedValue(new Error('Invalid XML'));
        await workflow.loadXml({ name: 'bad.xml', size: 1, text: async () => 'bad' });
        expect(workflow.xml).toBe('');
        expect(workflow.status).toBe('Invalid XML');
        const text = vi.fn(async () => 'oversized');
        await workflow.loadXml({ name: 'large.xml', size: 262145, text });
        expect(text).not.toHaveBeenCalled();
        expect(service.list).not.toHaveBeenCalled();
        expect(service.connect).not.toHaveBeenCalled();
    });
});
