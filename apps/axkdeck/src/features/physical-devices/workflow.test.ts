import { describe, expect, it, vi } from 'vitest';
import { DeviceWorkflow } from './workflow.svelte';
import type { DeviceService, DeviceSession } from '../../lib/httpDevices';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';

const connection: DeviceSession = {
    sessionId: 'owned',
    candidate: { id: 'candidate', model: 'Test', serial: '1', sizeBytes: 1024, sectorSize: 512 },
    targets: [
        {
            id: 'disk',
            label: 'Disk',
            sizeBytes: 1024,
            offsetBytes: 0,
            scsi: null,
            sectorSize: 512,
            readOnly: false,
            notices: [],
        },
    ],
    writesSupported: true,
    writesAuthorized: false,
    invalidated: false,
};
function fixture() {
    const service = {
        available: vi.fn(async () => true),
        inspectXml: vi.fn(),
        list: vi.fn(async () => ({ devices: [connection.candidate], writesSupported: true })),
        connect: vi.fn(async () => connection),
        status: vi.fn(async () => connection),
        close: vi.fn(async () => {}),
        authorize: vi.fn(async () => {}),
        recover: vi.fn(async () => {}),
        operations: vi.fn(async () => ({ operations: [], uncertain: [] })),
    } satisfies DeviceService;
    const images = {
        location: { kind: 'device', reference: { sessionId: 'owned', targetId: 'disk' }, displayName: 'Test' },
        refresh: vi.fn(async () => {}),
        setStatus: vi.fn(),
        open: vi.fn(async () => {}),
    } as unknown as ImageSessionWorkflow;
    return { service, images, workflow: new DeviceWorkflow(service, images) };
}
describe('physical device workflow', () => {
    it('does not touch devices during construction and requires an explicit connection and target selection', async () => {
        const { workflow, service } = fixture();
        expect(service.list).not.toHaveBeenCalled();
        await workflow.show();
        expect(workflow.candidateId).toBe('');
        expect(service.connect).not.toHaveBeenCalled();
        workflow.candidateId = 'candidate';
        await workflow.connect();
        expect(workflow.targetId).toBe('');
        expect(service.authorize).not.toHaveBeenCalled();
        await workflow.close();
        expect(service.close).toHaveBeenCalledWith('owned');
    });
    it('never borrows or authorizes the active image connection for recovery', async () => {
        const { workflow, service } = fixture();
        await workflow.show();
        expect(workflow.connection).toBeNull();
        expect(service.status).not.toHaveBeenCalled();
        expect(service.authorize).not.toHaveBeenCalled();
        await workflow.close();
        expect(service.close).not.toHaveBeenCalled();
    });
    it('never reconnects or reopens automatically after explicit recovery', async () => {
        const { workflow, service, images } = fixture();
        workflow.mode = 'recovery';
        workflow.connection = connection;
        workflow.targetId = 'disk';
        workflow.recoveryId = 'interrupted';
        workflow.acknowledged = true;
        await workflow.recover();
        expect(service.recover).toHaveBeenCalledWith('interrupted', 'owned', 'disk');
        expect(service.close).toHaveBeenCalledWith('owned');
        expect(images.open).not.toHaveBeenCalled();
        expect(workflow.connection).toBeNull();
    });
});
