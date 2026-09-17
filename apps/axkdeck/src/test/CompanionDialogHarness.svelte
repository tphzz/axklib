<script lang="ts">
    import CompanionDiskDialog from '../lib/components/CompanionDiskDialog.svelte';
    import type { ImageLocation } from '../lib/storageLocations';
    let closed = $state(false);
    let busy = $state(false);
    let sources = $state<ImageLocation[]>(
        Array.from({ length: 24 }, (_, index) => ({
            kind: 'axk-object-directory',
            reference: { rootId: 'corpus', relativePath: `set/disk${index + 2}` },
            displayName: `disk${index + 2}`,
        })),
    );
</script>

{#if closed}<p>Closed</p>{:else}
    <CompanionDiskDialog
        {sources}
        sourceKind="directory"
        setLabel="Disk set"
        nextRequiredIndex={2}
        {busy}
        error=""
        onadd={() => {}}
        onnearby={() => {}}
        onremove={(source) => {
            sources = sources.filter((item) => item !== source);
        }}
        onconfirm={() => {
            busy = true;
        }}
        oncancel={() => {
            closed = true;
        }}
    />
{/if}
