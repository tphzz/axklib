<script lang="ts">
    import FloppyImportDialog from '../lib/components/FloppyImportDialog.svelte';
    import { floppyDialogFixture } from './floppyDialogFixture';
    import PackageComparisonHarness from './PackageComparisonHarness.svelte';
    import { capacityConflict } from './importCapacityFixture';
    const query = new URLSearchParams(window.location.search);
    const workflow = floppyDialogFixture(250, query.has('direct'), query.get('folder') ?? undefined);
    const comparison = new URLSearchParams(window.location.search).has('packages');
    if (query.has('recovery')) {
        const request = workflow.request!;
        request.inspection!.complete = false;
        request.inspection!.recoveryUsed = true;
        request.inspection!.requiresAcknowledgement = true;
        request.inspection!.issues = [
            { code: 'FLOPPY_IMPORT_RECOVERY', message: 'Only complete objects are available.' },
        ];
        request.inspection!.objects[0].exclusionReason = 'A required dependency cannot be imported.';
        request.inspection!.excludedFiles.push({
            memberName: 'disk2.ima',
            path: 'MISSING.099',
            sizeBytes: 200,
            reason: 'Cataloged sampler object has no supported object header.',
            unreadableObject: true,
        });
        request.selected = request.selected.filter((key) => key !== 'wave-0');
        request.plan = null;
        request.dirty = true;
    }
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
