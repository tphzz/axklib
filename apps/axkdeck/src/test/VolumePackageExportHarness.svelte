<script lang="ts">
    import { onMount } from 'svelte';
    import VolumePackageExportDialog from '../lib/components/VolumePackageExportDialog.svelte';
    import type { ImageSessionVolumePackageExportInspection } from '../lib/transport';

    type Scenario = 'failed' | 'inspecting' | 'empty' | 'remote';
    let scenario = $state<Scenario>('failed');
    let open = $state(true);
    let long = $state(false);
    const name = $derived(long ? 'A3000 ProSnd Library '.repeat(12).trim() : 'A3000 ProSnd Lib');
    const inspection = $derived<ImageSessionVolumePackageExportInspection>({
        imageId: 'image-1',
        revision: 1,
        sourceMediaKind: 'ISO9660',
        scopeId: 'partition-0',
        scopeName: name,
        defaultDirectoryName: 'A3000 packages',
        volumeCount: 15,
        exportableCount: scenario === 'empty' ? 0 : 15,
        emptyCount: scenario === 'empty' ? 15 : 0,
        volumes: [],
    });
    const error = $derived(
        scenario === 'failed'
            ? long
                ? 'Download archive cache entry cannot be removed. '.repeat(80).trim()
                : 'Download archive cache is unavailable. No files were saved.'
            : '',
    );

    onMount(() => {
        const fixture = {
            show(next: Scenario, extended = false) {
                scenario = next;
                long = extended;
                open = true;
            },
        };
        Object.assign(window, { volumePackageExportFixture: fixture });
        return () => {
            Reflect.deleteProperty(window, 'volumePackageExportFixture');
        };
    });
</script>

<button type="button" class="secondary-button">Background action</button>
{#if open}
    <VolumePackageExportDialog
        scopeName={name}
        {inspection}
        desktop={true}
        destinationFlow={scenario === 'remote' ? 'CHOOSER' : 'DIRECT_COMPUTER'}
        loading={scenario === 'inspecting'}
        {error}
        onworkspace={() => {}}
        onlocal={() => {}}
        onretry={() => (scenario = 'inspecting')}
        oncancel={() => (open = false)}
    />
{:else}
    <p>Export closed</p>
{/if}
