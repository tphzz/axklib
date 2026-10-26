import { openSelect, selectValue } from '../../test/selectControl';
import { fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import OpenSourceHarness from '../../test/OpenSourceHarness.svelte';
import { storagePickerFixture } from '../../test/storagePickerFixture';
import type { DeviceService, DeviceSession } from '../../lib/httpDevices';

function fixture() {
    const transport = storagePickerFixture();
    const candidate = { id: 'test', model: 'USB test', serial: 'unit-only', sizeBytes: 1024, sectorSize: 512 };
    const service = {
        available: vi.fn(async () => true),
        list: vi.fn(async () => ({
            devices: [candidate, { ...candidate, id: 'large', model: 'Large card', sizeBytes: 32_000_000_000 }],
            writesSupported: true,
        })),
        inspectXml: vi.fn(async () => ({
            targets: [
                {
                    id: '0',
                    label: 'SCSI ID 0',
                    enabled: true,
                    eligible: true,
                    sizeBytes: 1024,
                    sectorSize: 512,
                    readOnly: false,
                    notices: [] as string[],
                },
            ],
        })),
        connect: vi.fn(async (_id, layout): Promise<DeviceSession> => ({
            sessionId: 'owned',
            candidate,
            targets: [
                {
                    id: layout === 'SCSI2SD_XML' ? '0' : 'disk',
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
        })),
        close: vi.fn(),
        status: vi.fn(),
        authorize: vi.fn(),
        recover: vi.fn(),
        operations: vi.fn(),
    } satisfies DeviceService;
    return { transport, service };
}
describe('Open dialog', () => {
    it('retains immediate image source membership when switching Open tabs', async () => {
        const { transport, service } = fixture();
        const view = render(OpenSourceHarness, { transport, service });
        await fireEvent.click(await view.findByRole('option', { name: /Yamaha images/ }));
        await fireEvent.click(await view.findByRole('option', { name: /Disk02.hds/ }));
        await fireEvent.click(view.getByRole('tab', { name: 'Device' }));
        await fireEvent.click(view.getByRole('tab', { name: 'Image' }));
        expect(view.getByRole('option', { name: /Disk02.hds/ }).getAttribute('aria-selected')).toBe('true');
        const sources = within(view.getByRole('region', { name: 'Source set' }));
        expect(sources.getAllByRole('button')).toHaveLength(1);
        await fireEvent.click(sources.getByRole('button', { name: /Remove .*Disk02.hds/ }));
        expect(view.getByRole('option', { name: /Disk02.hds/ }).getAttribute('aria-selected')).toBe('false');
        expect(view.getByRole('button', { name: 'Open' })).toHaveProperty('disabled', true);
    });

    it('uses the shared dark field styling for every device and target selector', () => {
        const { transport, service } = fixture();
        const view = render(OpenSourceHarness, { transport, service });
        const selects = [...view.container.querySelectorAll('[role="combobox"]')];
        expect(selects).toHaveLength(5);
        for (const select of selects) expect(select.classList.contains('select-control')).toBe(true);
    });

    it('offers write access immediately and authorizes it as part of Open', async () => {
        const { transport, service } = fixture();
        const oncancel = vi.fn();
        const view = render(OpenSourceHarness, { transport, service, oncancel });
        const tab = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'test');
        const writeAccess = view.getByRole('button', { name: 'Read & write' });
        const open = view.getByRole('button', { name: 'Open' });
        expect(writeAccess.getAttribute('aria-pressed')).toBe('true');
        expect((open as HTMLButtonElement).disabled).toBe(true);
        const acknowledgement = view.getByRole('checkbox', { name: /No sampler or other SCSI host/ });
        expect((acknowledgement as HTMLInputElement).checked).toBe(false);
        await fireEvent.click(acknowledgement);
        expect((open as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(open);
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(service.authorize).toHaveBeenCalledWith('owned', 'disk', expect.any(Function));
        expect(view.queryByRole('dialog', { name: 'Authorize device writes' })).toBeNull();
    });

    it('allows explicit read-only browsing without an acknowledgement or write authorization', async () => {
        const { transport, service } = fixture();
        const oncancel = vi.fn();
        const view = render(OpenSourceHarness, { transport, service, oncancel });
        const tab = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'test');
        await fireEvent.click(view.getByRole('button', { name: 'Read only' }));
        expect(view.queryByRole('checkbox')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Open' }));
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(service.authorize).not.toHaveBeenCalled();
    });

    it('resets the acknowledgement after access changes and blocks dismissal during OS authorization', async () => {
        const { transport, service } = fixture();
        const oncancel = vi.fn();
        const view = render(OpenSourceHarness, { transport, service, oncancel });
        const tab = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'test');
        await fireEvent.click(view.getByRole('checkbox'));
        await fireEvent.click(view.getByRole('button', { name: 'Read only' }));
        await fireEvent.click(view.getByRole('button', { name: 'Read & write' }));
        expect((view.getByRole('checkbox') as HTMLInputElement).checked).toBe(false);
        await fireEvent.click(view.getByRole('checkbox'));
        let finish!: () => void;
        service.authorize.mockImplementation(() => new Promise<void>((resolve) => (finish = resolve)));
        await fireEvent.click(view.getByRole('button', { name: 'Open' }));
        await waitFor(() => expect(service.authorize).toHaveBeenCalledOnce());
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        await fireEvent.click(view.container.querySelector('.dialog-backdrop')!);
        expect((view.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
        expect(oncancel).not.toHaveBeenCalled();
        finish();
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('retains permission failures in Open and permits an explicit read-only retry', async () => {
        const { transport, service } = fixture();
        const oncancel = vi.fn();
        const view = render(OpenSourceHarness, { transport, service, oncancel });
        const tab = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'test');
        await fireEvent.click(view.getByRole('checkbox'));
        service.authorize.mockRejectedValue(new Error('Permission declined'));
        await fireEvent.click(view.getByRole('button', { name: 'Open' }));
        await within(view.getByRole('tabpanel')).findByText('Permission declined');
        expect(oncancel).not.toHaveBeenCalled();
        await waitFor(() => expect(service.close).toHaveBeenCalledWith('owned'));
        await fireEvent.click(view.getByRole('button', { name: 'Read only' }));
        await fireEvent.click(view.getByRole('button', { name: 'Open' }));
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        expect(service.authorize).toHaveBeenCalledOnce();
    });

    it('explains a disabled write option on unsupported platforms', async () => {
        const { transport, service } = fixture();
        service.list.mockResolvedValue({ devices: [], writesSupported: false });
        const view = render(OpenSourceHarness, { transport, service });
        const tab = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        const writeAccess = view.getByRole('button', { name: 'Read & write' }) as HTMLButtonElement;
        await waitFor(() => expect(writeAccess.disabled).toBe(true));
        expect(view.getByRole('button', { name: 'Read only' }).getAttribute('aria-pressed')).toBe('true');
        expect(within(view.getByRole('tabpanel')).getByText('Writing is unavailable on this platform.')).toBeTruthy();
        expect(writeAccess.getAttribute('aria-describedby')).toBe('open-access-reason-Device');
    });

    it('defaults to Image, keeps tab order, preserves browsing across tabs and leaves other pickers alone', async () => {
        const { transport, service } = fixture();
        const view = render(OpenSourceHarness, { transport, service });
        expect(view.getAllByRole('tab').map((tab) => tab.textContent)).toEqual([
            'Image',
            'Device',
            'SCSI2SDv6',
            'SCSI2SD XML',
        ]);
        await fireEvent.click(await view.findByText('Yamaha images'));
        await waitFor(() => expect(view.getByRole('button', { name: 'Add current folder' })).toBeTruthy());
        expect(service.list).not.toHaveBeenCalled();
        const device = view.getByRole('tab', { name: 'Device' });
        await waitFor(() => expect((device as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(device);
        await waitFor(() => expect(service.list).toHaveBeenCalledOnce());
        expect((view.getByRole('combobox', { name: 'Device' }) as HTMLButtonElement).value).toBe('');
        expect(view.queryByRole('option', { name: /Large card/ })).toBeNull();
        await fireEvent.keyDown(device, { key: 'ArrowLeft' });
        expect(view.getByRole('tab', { name: 'Image' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Add current folder' })).toBeTruthy();
    });
    it('keeps device tabs disabled with an accessible reason without a local backend', async () => {
        const view = render(OpenSourceHarness, { transport: storagePickerFixture() });
        const device = view.getByRole('tab', { name: 'Device' });
        expect((device as HTMLButtonElement).disabled).toBe(true);
        expect(device.getAttribute('aria-describedby')).toBe('open-device-unavailable');
        expect(view.getByText('Device access requires the local desktop backend.')).toBeTruthy();
    });
    it('validates XML before enabling the card selector and requires an explicit SCSI target', async () => {
        const { transport, service } = fixture();
        const view = render(OpenSourceHarness, { transport, service });
        const tab = view.getByRole('tab', { name: 'SCSI2SD XML' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        expect((view.getByRole('combobox', { name: 'Device' }) as HTMLButtonElement).disabled).toBe(true);
        expect(service.list).not.toHaveBeenCalled();
        const file = { name: 'test.xml', size: 4, text: async () => 'xml' };
        await fireEvent.change(view.getByLabelText('Configuration XML'), { target: { files: [file] } });
        await waitFor(() => expect(service.inspectXml).toHaveBeenCalledWith('xml'));
        await waitFor(() => expect(service.list).toHaveBeenCalledOnce());
        await openSelect(view.getByRole('combobox', { name: 'Device' }));
        expect(view.getByRole('option', { name: /Large card/ })).toBeTruthy();
        expect((view.getByRole('combobox', { name: 'SCSI target' }) as HTMLButtonElement).value).toBe('');
        expect((view.getByRole('button', { name: 'Open' }) as HTMLButtonElement).disabled).toBe(true);
    });
    it('dismisses with Escape and outside click through the same cancellation path', async () => {
        const oncancel = vi.fn();
        const view = render(OpenSourceHarness, { transport: storagePickerFixture(), oncancel });
        await fireEvent.keyDown(view.getByRole('dialog'), { key: 'Escape' });
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
        view.unmount();
        const second = render(OpenSourceHarness, { transport: storagePickerFixture(), oncancel });
        await fireEvent.click(second.container.querySelector('.dialog-backdrop')!);
        await waitFor(() => expect(oncancel).toHaveBeenCalledTimes(2));
    });
    it('automatically previews the selected V6 carrier, displays exact metadata and keeps optical access read-only', async () => {
        const { transport, service } = fixture();
        const connection: DeviceSession = {
            sessionId: 'v6-preview',
            candidate: {
                id: 'large',
                model: 'Large card',
                serial: 'unit-only',
                sizeBytes: 32_000_000_000,
                sectorSize: 512,
            },
            targets: [
                {
                    id: '2',
                    label: 'SCSI ID 2',
                    sizeBytes: 2048000,
                    offsetBytes: 1536000,
                    sectorSize: 2048,
                    readOnly: true,
                    notices: [],
                    scsi: {
                        id: 2,
                        deviceType: 'OPTICAL',
                        vendor: 'codesrc',
                        product: 'CD-ROM',
                        revision: '6.0',
                        serial: 'target-2',
                    },
                },
            ],
            writesSupported: true,
            writesAuthorized: false,
            invalidated: false,
        };
        service.connect.mockResolvedValue(connection);
        service.status.mockResolvedValue(connection);
        const view = render(OpenSourceHarness, { transport, service });
        const tab = view.getByRole('tab', { name: 'SCSI2SDv6' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        expect(service.connect).not.toHaveBeenCalled();
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'large');
        await waitFor(() =>
            expect((view.getByRole('combobox', { name: 'SCSI target' }) as HTMLButtonElement).disabled).toBe(false),
        );
        await openSelect(view.getByRole('combobox', { name: 'SCSI target' }));
        expect(view.getByRole('option', { name: /SCSI ID 2 \/ CD-ROM.*\[1536000, 3584000\)/ })).toBeTruthy();
        expect(service.connect).toHaveBeenCalledWith('large', 'SCSI2SD_V6');
        expect((view.getByRole('button', { name: 'Open' }) as HTMLButtonElement).disabled).toBe(true);
        await selectValue(view.getByRole('combobox', { name: 'SCSI target' }), '2');
        const details = within(view.getByLabelText('Configured target details'));
        expect(details.getByText('3584000')).toBeTruthy();
        expect(details.getByText('2048 B')).toBeTruthy();
        expect(details.getByText('codesrc CD-ROM')).toBeTruthy();
        expect(details.getByText('6.0 / target-2')).toBeTruthy();
        expect((view.getByRole('button', { name: 'Read & write' }) as HTMLButtonElement).disabled).toBe(true);
        expect((view.getByRole('button', { name: 'Open' }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Refresh devices' }));
        await waitFor(() => expect(service.status).toHaveBeenCalledOnce());
        expect((view.getByRole('combobox', { name: 'SCSI target' }) as HTMLButtonElement).value).toBe('2');
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        await waitFor(() => expect(service.close).toHaveBeenCalledWith('v6-preview'));
        expect(service.authorize).not.toHaveBeenCalled();
    });
    it('shows invalid configuration failures without stale targets or automatic authorization', async () => {
        const { transport, service } = fixture();
        service.connect.mockRejectedValue(new Error('The SCSI2SDv6 configuration has invalid target geometry.'));
        const view = render(OpenSourceHarness, { transport, service });
        const tab = view.getByRole('tab', { name: 'SCSI2SDv6' });
        await waitFor(() => expect((tab as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(tab);
        await selectValue(view.getByRole('combobox', { name: 'Device' }), 'test');
        await within(view.getByRole('tabpanel')).findByText(/invalid target geometry/);
        const targets = view.getByRole('combobox', { name: 'SCSI target' }) as HTMLButtonElement;
        expect(targets.disabled).toBe(true);
        expect(targets.textContent?.trim()).toBe('Select a configured target');
        expect((view.getByRole('button', { name: 'Open' }) as HTMLButtonElement).disabled).toBe(true);
        expect(service.authorize).not.toHaveBeenCalled();
    });
});
