<script lang="ts">
    import { untrack } from 'svelte';
    import type { ImageTransport } from '../../lib/transport';
    import type { ConversionProfile } from '../../lib/conversions';
    import { saveClientDownload } from '../../lib/clientFiles';
    import { saveRetainedMedia, selectLocalMediaDestination } from '../../lib/nativeMediaExports';
    import { PickerController, type PickerRequest } from '../dialogs/picker';
    import PickerDialogHost from '../dialogs/PickerDialogHost.svelte';
    import { waveDiskRoutes, WaveDiskWorkflow } from './waveDiskWorkflow.svelte';
    import { waveSourceScope } from './waveSourceScope';
    import type { DiskTreeItem } from '../../lib/types';
    let {
        transport,
        sessionId,
        documentId,
        revision,
        sourceFormat,
        sourceName,
        isDesktop,
        setStatus,
        manageLocations,
    }: {
        transport: ImageTransport;
        sessionId: number;
        documentId: string;
        revision: number;
        sourceFormat: string;
        sourceName: string;
        isDesktop: boolean;
        setStatus: (value: string) => void;
        manageLocations: () => void;
    } = $props();
    let profiles = $state<ConversionProfile[]>([]);
    let pickerRequest = $state<PickerRequest | null>(null);
    const picker = new PickerController((r) => (pickerRequest = r));
    const workflow = new WaveDiskWorkflow(
        untrack(() => transport),
        async (download, filename) => {
            if (isDesktop) {
                const destination = await selectLocalMediaDestination(filename);
                if (!destination) return false;
                await saveRetainedMedia(destination.candidateId, download.contentPath, download.sizeBytes);
            } else {
                saveClientDownload({ filename, blob: await transport.readConversionDownload(download) });
            }
            return true;
        },
        (status) => setStatus(status),
    );
    $effect(() => {
        const current = transport;
        let active = true;
        void (async () => {
            try {
                const all = await current.conversionProfiles();
                if (active) profiles = all;
            } catch {
                if (active) profiles = [];
            }
        })();
        return () => {
            active = false;
            picker.dispose();
            workflow.close();
        };
    });
    export function available() {
        return waveDiskRoutes(profiles, sourceFormat).length > 0;
    }
    export function isOpen() {
        return !!workflow.request;
    }
    export function open(initial: string[], item?: DiskTreeItem) {
        return workflow.open(
            { kind: 'DOCUMENT', sessionId, documentId, expectedRevision: revision },
            sourceName,
            sourceFormat,
            profiles,
            initial,
            waveSourceScope(transport, sessionId, item),
        );
    }
    async function browse() {
        const request = workflow.request;
        if (!request || workflow.locked) return;
        const file = await picker.chooseLocation('save-file', 'Create Yamaha Wave disk', ['img'], 'Waves.img', {
            parentDialog: 'conversion-destination',
            requireWritableDirectory: true,
            initialFile: request.delivery.kind === 'server' ? request.delivery.file?.reference : null,
        });
        if (file?.kind === 'server-file' && workflow.request?.source.documentId === request.source.documentId)
            workflow.change({ delivery: { kind: 'server', file } });
    }
</script>

{#if workflow.request}
    {#await import('./WaveDiskDialog.svelte') then dialog}
        <dialog.default {workflow} request={workflow.request} {browse} />
    {/await}
{/if}
<PickerDialogHost
    {transport}
    request={pickerRequest}
    finish={(selection) => picker.finish(selection)}
    {manageLocations}
/>
