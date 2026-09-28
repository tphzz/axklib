<script lang="ts">
    import ObjectDeletionDialog from '../lib/components/ObjectDeletionDialog.svelte';
    import type { ObjectDeletionInspection } from '../lib/transport';

    let open = $state(true);
    let busy = $state(false);
    let error = $state('');
    let attempts = $state(0);
    const impacts: ObjectDeletionInspection['impacts'] = Array.from({ length: 119 }, (_, index) => ({
        objectId: `object-${index}`,
        objectType: index === 0 ? 'PROG' : 'SBNK',
        objectName: index === 0 ? '009: T.MORIO' : `Sample ${index}`,
        partitionIndex: 0,
        partitionName: 'PARTITION 1',
        volumeName: 'bench',
        role: index === 0 ? 'TARGET' : 'DEPENDENCY',
        status: index === 0 ? 'REQUIRED' : 'OPTIONAL',
        requested: true,
        selected: true,
        storedSizeBytes: 512,
        freedClusters: 2,
        prerequisiteObjectIds: index === 0 ? [] : ['object-0'],
        reason: '',
    }));
    const inspection: ObjectDeletionInspection = {
        canApply: true,
        imageId: 'image',
        revision: 1,
        targetObjectIds: ['object-0'],
        referrerObjectIds: [],
        cleanupObjectIds: impacts.slice(1).map((impact) => impact.objectId),
        selectedObjectIds: impacts.map((impact) => impact.objectId),
        impacts,
        references: [],
        blockers: [],
        warnings: [],
        estimatedFreedBytes: 243712,
        estimatedFreedClusters: 238,
    };

    async function confirm() {
        if (busy) return;
        attempts++;
        busy = true;
        error = '';
        await new Promise((resolve) => setTimeout(resolve, 100));
        busy = false;
        error =
            'SFS extent byte total cannot equal the logical record payload size. The image has been refreshed; review the deletion again.';
    }
</script>

<button
    onclick={() => {
        open = true;
        error = '';
    }}>Open deletion</button
>
<output data-testid="attempts">{attempts}</output>
{#if open}
    <ObjectDeletionDialog
        {inspection}
        loading={false}
        {busy}
        {error}
        onselectionchange={() => {}}
        onselectall={() => {}}
        oncancel={() => (open = false)}
        onconfirm={() => void confirm()}
    />
{/if}
