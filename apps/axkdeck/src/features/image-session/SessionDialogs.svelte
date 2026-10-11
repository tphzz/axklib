<script lang="ts">
    import type { ImageSessionWorkflow } from './workflow.svelte';
    import type { ExtentLayoutRepairWorkflow } from './extentLayoutRepairWorkflow.svelte';
    import ImageIntegrityDialog from '../../lib/components/ImageIntegrityDialog.svelte';
    import ImageOpenProgressDialog from '../../lib/components/ImageOpenProgressDialog.svelte';
    let { workflow, repairs }: { workflow: ImageSessionWorkflow; repairs: ExtentLayoutRepairWorkflow } = $props();
</script>

{#if workflow.integrityDialogOpen}
    <ImageIntegrityDialog
        issues={workflow.integrityIssues}
        loading={workflow.integrityLoading}
        error={workflow.integrityError}
        repairAvailable={workflow.extentLayoutRepairAvailable}
        repairing={repairs.busy}
        repairLabel={repairs.progressLabel}
        repairError={repairs.error}
        onrepair={() => void repairs.repair()}
        onclose={() => workflow.closeIntegrity()}
    />
{/if}
{#if workflow.openProgressVisible}
    <ImageOpenProgressDialog
        label={workflow.openProgressLabel}
        completed={workflow.openProgressCompleted}
        total={workflow.openProgressTotal}
        cancellable={workflow.openProgressCancellable}
        cancelling={workflow.openProgressCancelling}
        oncancel={() => workflow.cancelOpen()}
    />
{/if}
