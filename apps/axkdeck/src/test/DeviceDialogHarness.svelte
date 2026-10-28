<script lang="ts">
    import { onMount } from 'svelte';
    import { deviceIdentity } from './deviceFixture';
    import DeviceDialog from '../features/physical-devices/DeviceDialog.svelte';
    import { provideDevices } from '../features/physical-devices/workflow.svelte';
    import OpenSourceDialog from '../features/dialogs/OpenSourceDialog.svelte';
    import { storagePickerFixture } from './storagePickerFixture';
    import type { DeviceService, DeviceSession } from '../lib/httpDevices';
    import type { ImageLocation } from '../lib/storageLocations';
    import type { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
    import type { ImageSessionPreparation } from '../features/image-session/actions';
    import type { OpenedImage } from '../lib/transport';
    import { CreateWorkflow } from '../features/create/workflow.svelte';
    import CreateDialog from '../features/create/CreateDialog.svelte';
    import { deviceFormatInspection, deviceFormatSession } from './deviceFormatFixture';
    import { serverDirectoryLocation } from '../lib/storageLocations';

    const candidate = {
        identity: deviceIdentity,
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
                recoveryConfirmationText: 'RESTORE /dev/synthetic',
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
    const createSession = deviceFormatSession();
    let currentCreateSession = createSession;
    let formatOperation = '';
    const inspectCreate = () => {
        const inspected = deviceFormatInspection(currentCreateSession, currentCreateSession.targets[0]!.id);
        inspected.contents = Array.from({ length: 80 }, (_, index) => ({
            ...inspected.contents[index % 2]!,
            label: `Synthetic old partition ${index + 1}`,
        }));
        if (mode === 'create-mounted') {
            inspected.blockedReasons = ['Unmount all host volumes using the operating system, then inspect again.'];
            inspected.mountedVolumes = [
                {
                    device: '/dev/synthetic1',
                    mountPoint: '/media/Synthetic card',
                    filesystem: 'FAT32',
                    label: 'OLD DATA',
                },
            ];
        }
        return inspected;
    };
    const service: DeviceService = {
        formatting: {
            available: async () => true,
            inspect: async () => inspectCreate(),
            plan: async (_session, _token, count) => ({
                planToken: 'synthetic-plan',
                inspection: inspectCreate(),
                partitionCount: count,
                expiresInSeconds: 900,
            }),
            start: async (_input, operationId) => {
                calls += 'format ';
                formatOperation = operationId;
                if (mode === 'create-unconfirmed') throw new Error('Synthetic lost acknowledgement');
                return { jobId: 1, serverJobId: 'synthetic-format', kind: 'devices.sfs.format', status: 'queued' };
            },
            wait: async () => ({
                jobId: 1,
                serverJobId: 'synthetic-format',
                kind: 'devices.sfs.format',
                status: 'completed',
                result: {
                    operationId: formatOperation,
                    sessionId: currentCreateSession.sessionId,
                    targetId: currentCreateSession.targets[0]!.id,
                    sizeBytes: inspectCreate().formattedSizeBytes,
                    partitionCount: 8,
                },
            }),
            status: async () => {
                throw new Error('No synthetic job is retained');
            },
            cancel: async () => {},
            resolve: async (operationId) => ({
                operationId,
                kind: 'FORMAT',
                state: 'NO_WRITE',
                model: createSession.candidate.model,
                serial: createSession.candidate.serial,
                targetLabel: 'Whole disk',
                targetSize: createSession.candidate.sizeBytes,
            }),
        },
        available: async () => true,
        inspectXml: async () => ({
            targets: [{ ...connection.targets[0]!, id: '0', label: 'SCSI ID 0', enabled: true, eligible: true }],
        }),
        list: async () => {
            calls += 'list ';
            return {
                devices: mode?.startsWith('create-') ? [createSession.candidate] : mode === 'empty' ? [] : [candidate],
                writesSupported: true,
            };
        },
        connect: async (_id, layout) => {
            if (mode?.startsWith('create-')) {
                currentCreateSession =
                    layout === 'RAW'
                        ? createSession
                        : {
                              ...createSession,
                              targets: [
                                  {
                                      ...createSession.targets[0]!,
                                      id: '0',
                                      label: 'SCSI ID 0',
                                      offsetBytes: 1048576,
                                      sizeBytes: 3_000_000_000,
                                      scsi: {
                                          id: 0,
                                          deviceType: 'FIXED',
                                          vendor: 'codesrc',
                                          product: 'Synthetic target',
                                          revision: '6.0',
                                          serial: 'TARGET-TEST',
                                      },
                                      recoveryConfirmationText: 'RESTORE /dev/synthetic SCSI 0',
                                  },
                              ],
                          };
                return currentCreateSession;
            }
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
        confirmEditorLeave: async () => true,
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
    transport.hardDiskCreationProfiles = async () => [
        {
            profileId: 'FLOPPY_SCALE',
            sizeBytes: 1_474_560,
            defaultPartitionCount: 1,
            partitionOptions: [{ partitionCount: 1, partitionSizeBytes: 1_473_024, unusedTailBytes: 0 }],
        },
    ];
    const creation = new CreateWorkflow(service, images);
    let opening = $state(true);
    const mode = new URLSearchParams(location.search).get('mode');
    onMount(() => {
        if (mode?.startsWith('create-')) void creation.show();
    });
    workflow.mode = mode === 'recovery' ? mode : null;
    workflow.candidates = mode === 'empty' ? [] : [candidate];
    if (workflow.mode === 'recovery')
        workflow.operations = Array.from({ length: 40 }, (_, index) => ({
            kind: 'EDIT' as const,
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
{#if !workflow.mode && opening && mode !== 'recovery' && !mode?.startsWith('create-')}
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
{#if creation.visible}<CreateDialog
        workflow={creation}
        {transport}
        directory={serverDirectoryLocation({ rootId: 'workspace', relativePath: 'images' }, 'Synthetic images')}
        onbrowse={() => {}}
        onsuccess={() => {}}
        onrecovery={() => {}}
    />{/if}
