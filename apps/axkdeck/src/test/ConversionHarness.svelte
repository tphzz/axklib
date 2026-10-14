<script lang="ts">
    import Tx16wImportDialog from '../lib/components/Tx16wImportDialog.svelte';
    import { ImportCompletion } from '../features/import/importCompletion.svelte';
    import { JobController } from '../features/jobs/actions';
    import type { Tx16wImportRequest } from '../features/import/tx16wWorkflow.svelte';
    import type { ConversionPlan } from '../lib/conversions';
    import { serverFileLocation } from '../lib/storageLocations';
    import { fitsCapacity } from './samplerCapacityFixture';

    const target = { partitionIndex: 0, volumeName: 'Samples' };
    const native = new URLSearchParams(location.search).has('native');
    const plan: ConversionPlan = {
        planId: 'conversion-layout',
        state: 'READY',
        expiresAt: Date.now() + 900000,
        jobId: null,
        report: {
            profileId: 'yamaha.tx16w-to-a-series',
            fidelity: 'BEST',
            executable: true,
            actionCount: 201,
            issueCount: 2,
            offset: 0,
            capacity: fitsCapacity,
            destination: { imageId: 'image-1', documentId: 'document-1', expectedRevision: 1 },
            actions: Array.from({ length: 201 }, (_, index) => ({
                kind: 'wave-data',
                action: 'create',
                sourceObject: `native-wave-${index}`,
                name: `Concert-Piano-${index}`,
                details: [
                    { label: 'Sample rate', value: '48000' },
                    { label: 'Loop start', value: '122400' },
                ],
            })),
            issues: [
                {
                    id: 'loss',
                    code: 'resampling',
                    impact: 'LOSS',
                    message: 'Wave Data resampled to 48000 Hz',
                    sourceObject: 'native-wave-1',
                    sourceParameter: 'sample_rate',
                    targetObject: 'Concert-Piano-1',
                    targetParameter: 'sample_rate',
                },
                {
                    id: 'default',
                    code: 'defaults',
                    impact: 'UNCERTAINTY',
                    message: 'Destination controls use writer defaults',
                    sourceObject: 'native-wave-1',
                    sourceParameter: '',
                    targetObject: 'Concert-Piano-1',
                    targetParameter: '',
                },
            ],
        },
    };
    let open = $state(true);
    let request = $state<Tx16wImportRequest>({
        target,
        importMode: 'HIERARCHY',
        plan,
        fidelity: 'BEST',
        acknowledgedIssueIds: [],
        status: 'ready',
        progress: 1,
        error: '',
        documentSource: native
            ? { kind: 'DOCUMENT', sessionId: 1, documentId: 'tx16w', expectedRevision: 1 }
            : undefined,
        destinationName: native ? 'Target image.hds' : undefined,
        members: native
            ? []
            : Array.from({ length: 24 }, (_, index) => ({
                  id: index,
                  source: serverFileLocation({
                      rootId: 'root',
                      relativePath: `TX16W-Concert-Library-Disk-${index}.ima`,
                  }),
                  sourceName: `TX16W-Concert-Library-Disk-${index}.ima`,
                  resolvedSource: null,
                  upload: null,
              })),
    });
    const jobs = {
        waitForJob: async () => ({ jobId: 1, kind: 'conversions.execute', status: 'completed' as const }),
        cancelJob: async () => undefined,
    };
    const completion = new ImportCompletion(jobs, new JobController(jobs));
    function fidelity(value: 'BEST' | 'STRICT') {
        request.fidelity = value;
        request.acknowledgedIssueIds = [];
        request.plan = { ...plan, report: { ...plan.report!, fidelity: value, executable: value === 'BEST' } };
    }
</script>

{#if open}
    <Tx16wImportDialog
        {request}
        {completion}
        volumeOptions={[{ target, key: '0:Samples', label: 'Disk - Samples', partitionName: 'Disk' }]}
        onrecover={() => {
            completion.phase = 'completed';
            open = false;
        }}
        ontarget={(value) => {
            request.target = value;
        }}
        onmode={(value) => {
            request.importMode = value;
        }}
        onfidelity={fidelity}
        onprepare={() => fidelity(request.fidelity)}
        onadd={() => undefined}
        onremove={(id) => {
            request.members = request.members.filter((item) => item.id !== id);
        }}
        onconfirm={() => {
            completion.phase = 'refresh-failed';
            completion.message = 'Import saved; refresh failed';
        }}
        oncancel={() => {
            open = false;
        }}
    />
{:else}
    <p>Conversion closed</p>
{/if}
