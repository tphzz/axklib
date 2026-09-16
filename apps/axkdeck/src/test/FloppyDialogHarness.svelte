<script lang="ts">
    import FloppyImportDialog from '../lib/components/FloppyImportDialog.svelte';
    import { floppyDialogFixture } from './floppyDialogFixture';
    import PackageComparisonHarness from './PackageComparisonHarness.svelte';
    import { capacityConflict } from './importCapacityFixture';
    const workflow = floppyDialogFixture(250, new URLSearchParams(window.location.search).has('direct'));
    const comparison = new URLSearchParams(window.location.search).has('packages');
    if (new URLSearchParams(window.location.search).has('capacity')) {
        workflow.request!.plan!.valid = false;
        workflow.request!.plan!.conflicts = Array.from({ length: 63 }, (_, index) =>
            capacityConflict(0, `Wave ${index}`),
        );
    }
</script>

{#if comparison}<PackageComparisonHarness />{:else if workflow.request}<FloppyImportDialog {workflow} />{:else}<p>
        Closed
    </p>{/if}
