<script lang="ts">
    import { onMount } from 'svelte';
    import WaveDiskDialog from '../features/conversion/WaveDiskDialog.svelte';
    import { WaveDiskWorkflow } from '../features/conversion/waveDiskWorkflow.svelte';
    import { serverFileLocation } from '../lib/storageLocations';
    import { waveFixtureTransport, waveSource, waveProfile, waveRows } from './waveDiskFixture';
    const transport = waveFixtureTransport();
    const output = serverFileLocation({ rootId: 'exports', relativePath: 'Studio.img' }, 'exports/Studio.img');
    let downloaded = $state('');
    const workflow = new WaveDiskWorkflow(
        transport,
        async (_, name) => {
            downloaded = name;
            return true;
        },
        () => {},
    );
    onMount(() => {
        void workflow
            .open(waveSource, 'Keys.hds', 'yamaha.a-series', [waveProfile], ['wave-0'], {
                kind: 'volume',
                label: 'Partition 1 / Studio',
                locations: async () => new Map(waveRows.map((row) => [row.id, `Partition 1 / Studio / ${row.name}`])),
            })
            .then(() => {
                workflow.change({ delivery: { kind: 'server', file: output } });
            });
        return () => workflow.close();
    });
</script>

{#if workflow.request}<WaveDiskDialog
        {workflow}
        request={workflow.request}
        browse={() => workflow.change({ delivery: { kind: 'server', file: output } })}
    />{/if}
{#if downloaded}<output>{downloaded}</output>{/if}
