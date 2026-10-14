<script lang="ts">
    import DeviceDialog from '../features/physical-devices/DeviceDialog.svelte';
    import { provideDevices } from '../features/physical-devices/workflow.svelte';
    import OpenSourceDialog from '../features/dialogs/OpenSourceDialog.svelte';
    import { storagePickerFixture } from './storagePickerFixture';
    import type { DeviceService, DeviceSession } from '../lib/httpDevices';
    import type { ImageLocation } from '../lib/storageLocations';
    import type { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
    import type { ImageSessionPreparation } from '../features/image-session/actions';
    import type { OpenedImage } from '../lib/transport';

    const candidate = {
        id: 'fixture',
        model: 'Synthetic device',
        serial: 'TEST-ONLY',
        sizeBytes: 4194304,
        sectorSize: 512,
    };
    const connection: DeviceSession = {
        sessionId: 'fixture-owned',
        candidate,
        targets: [
            {
                id: 'disk',
                label: 'Synthetic SFS target',
                sizeBytes: 4194304,
                offsetBytes: 1048576,
                scsi: {
                    id: 0,
                    deviceType: 'FIXED',
                    vendor: 'codesrc',
                    product: 'Synthetic target',
                    revision: '6.0',
                    serial: 'TARGET-TEST',
                },
                sectorSize: 512,
                readOnly: false,
                notices: [],
            },
        ],
        writesSupported: true,
        writesAuthorized: false,
        invalidated: false,
    };
    let calls = $state('');
    const service: DeviceService = {
        available: async () => true,
        inspectXml: async () => ({
            targets: [{ ...connection.targets[0]!, id: '0', label: 'SCSI ID 0', enabled: true, eligible: true }],
        }),
        list: async () => {
            calls += 'list ';
            return { devices: [candidate], writesSupported: true };
        },
        connect: async (_id, layout) => {
            if (mode === 'v6-invalid') throw new Error('No SCSI2SDv6 configuration found in the final two sectors.');
            return { ...connection, targets: [{ ...connection.targets[0]!, id: layout === 'RAW' ? 'disk' : '0' }] };
        },
        status: async () => ({ ...connection, targets: [{ ...connection.targets[0]!, id: '0' }] }),
        close: async () => {
            calls += 'close ';
        },
        authorize: async () => {
            calls += 'authorize ';
            if (mode === 'denied') throw new Error('Permission declined');
        },
        recover: async () => {
            calls += 'recover ';
        },
        operations: async () => ({ operations: [], uncertain: [] }),
    };
    const images = {
        location: null as ImageLocation | null,
        open: async ([location]: ImageLocation[], _preferred: unknown, prepare?: ImageSessionPreparation) => {
            await prepare?.({ sessionId: 1, format: 'sfs' } as OpenedImage);
            images.location = location;
            calls += 'open ';
        },
        cleanupOpen: async () => {},
        refresh: async () => {},
        setStatus: () => {},
    } as unknown as ImageSessionWorkflow;
    const workflow = provideDevices(service, images)!;
    const transport = storagePickerFixture(true);
    let opening = $state(true);
    const mode = new URLSearchParams(location.search).get('mode');
    workflow.mode = mode === 'recovery' ? mode : null;
    workflow.candidates = [candidate];
    if (workflow.mode === 'recovery')
        workflow.operations = Array.from({ length: 40 }, (_, index) => ({
            operationId: `synthetic-operation-${index}`,
            state: 'RECOVERY_REQUIRED',
            model: candidate.model,
            serial: candidate.serial,
            targetLabel: connection.targets[0]!.label,
            targetSize: candidate.sizeBytes,
        }));
</script>

<output aria-label="Fixture calls">{calls}</output>
{#if workflow.mode}<DeviceDialog {workflow} />{/if}
{#if !workflow.mode && opening && mode !== 'recovery'}
    <OpenSourceDialog
        {transport}
        mode="media-source"
        extensions={['hds', 'iso']}
        onselect={() => {}}
        onselectsources={() => {
            opening = false;
            calls += 'image ';
        }}
        oncancel={() => {
            opening = false;
        }}
    />
{/if}
