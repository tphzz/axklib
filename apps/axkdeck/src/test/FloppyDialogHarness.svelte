<script lang="ts">
    import FloppyImportDialog from '../lib/components/FloppyImportDialog.svelte';
    import { floppyDialogFixture } from './floppyDialogFixture';
    import PackageComparisonHarness from './PackageComparisonHarness.svelte';
    const workflow = floppyDialogFixture(250, new URLSearchParams(window.location.search).has('direct'));
    const comparison = new URLSearchParams(window.location.search).has('packages');
</script>

{#if comparison}<PackageComparisonHarness />{:else if workflow.request}<FloppyImportDialog {workflow} />{:else}<p>
        Closed
    </p>{/if}
