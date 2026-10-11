<script lang="ts">
    import { untrack } from 'svelte';
    import OpenSourceDialog from '../features/dialogs/OpenSourceDialog.svelte';
    import { provideDevices } from '../features/physical-devices/workflow.svelte';
    import type { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
    import type { ImageSessionPreparation } from '../features/image-session/actions';
    import type { OpenedImage } from '../lib/transport';
    import type { DeviceService } from '../lib/httpDevices';
    import type { ImageTransport } from '../lib/transport';
    import type { ImageLocation, DirectoryLocation } from '../lib/storageLocations';
    let {
        service,
        transport,
        onselect = () => {},
        oncancel = () => {},
        onopen = async () => true,
    }: {
        service?: DeviceService;
        transport: ImageTransport;
        onselect?: (location: ImageLocation | DirectoryLocation) => void;
        oncancel?: () => void;
        onopen?: (location: ImageLocation) => Promise<boolean>;
    } = $props();
    const images = {
        location: null as ImageLocation | null,
        status: 'Ready',
        open: async (location: ImageLocation, _preferred: unknown, prepare?: ImageSessionPreparation) => {
            if (await onopen(location)) {
                try {
                    await prepare?.({ sessionId: 1, format: 'sfs' } as OpenedImage);
                    images.location = location;
                } catch (error) {
                    images.status = error instanceof Error ? error.message : String(error);
                }
            }
        },
        cleanupOpen: async () => {},
    } as ImageSessionWorkflow;
    provideDevices(
        untrack(() => service),
        images,
    );
</script>

<OpenSourceDialog {transport} mode="media-source" extensions={['hds', 'iso']} {onselect} {oncancel} />
