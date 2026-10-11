import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import DeviceDialog from './DeviceDialog.svelte';
import { DeviceWorkflow } from './workflow.svelte';
import type { DeviceService } from '../../lib/httpDevices';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
function fixture() {
    const service = { close: vi.fn(async () => {}) } as unknown as DeviceService;
    const workflow = new DeviceWorkflow(service, {} as ImageSessionWorkflow);
    workflow.mode = 'recovery';
    workflow.candidates = [
        { id: 'small', model: 'Test', serial: '1', sizeBytes: 1024, sectorSize: 512 },
        { id: 'carrier', model: 'Large carrier', serial: '2', sizeBytes: 32_000_000_000, sectorSize: 512 },
    ];
    return { workflow, service };
}
describe('device dialog safety controls', () => {
    it('has no preselected device and exposes large carriers only for sliced layouts', async () => {
        const { workflow } = fixture();
        const view = render(DeviceDialog, { workflow });
        expect((view.getByLabelText('Device') as HTMLSelectElement).value).toBe('');
        expect((view.getByRole('button', { name: 'Connect read-only' }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.queryByRole('option', { name: /Large carrier/ })).toBeNull();
        await fireEvent.change(view.getByLabelText('Layout'), { target: { value: 'SCSI2SD_V6' } });
        expect(view.getByRole('option', { name: /Large carrier/ })).not.toBeNull();
    });
    it('releases an unopened connection on cancellation but blocks dismissal while busy', async () => {
        const { workflow, service } = fixture();
        workflow.connection = {
            sessionId: 'owned',
            candidate: workflow.candidates[0]!,
            targets: [],
            writesSupported: true,
            writesAuthorized: false,
            invalidated: false,
        };
        workflow.busy = true;
        const view = render(DeviceDialog, { workflow });
        expect((view.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
        workflow.busy = false;
        await waitFor(() =>
            expect((view.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(false),
        );
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        await waitFor(() => expect(service.close).toHaveBeenCalledWith('owned'));
    });
});
