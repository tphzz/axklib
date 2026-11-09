<script lang="ts">
    import Tx16wImportDialog from '../lib/components/Tx16wImportDialog.svelte';
    import { ImportCompletion } from '../features/import/importCompletion.svelte';
    import { JobController } from '../features/jobs/actions';
    import type { Tx16wImportRequest } from '../features/import/tx16wWorkflow.svelte';
    import type { ConversionPlan } from '../lib/conversions';
    import { serverFileLocation } from '../lib/storageLocations';
    import { fitsCapacity } from './samplerCapacityFixture';
    import { diskLayoutCapacity, diskLayoutCompletion } from './diskImportLayoutFixture';
    import { idleCompanionSearch } from '../features/import/tx16wCompanions';

    const target = { partitionIndex: 0, volumeName: 'Samples' };
    const query = new URLSearchParams(location.search);
    const native = query.has('native') || query.get('scenario') === 'native-long';
    const scenario = query.get('scenario') ?? 'normal';
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
        discovery: idleCompanionSearch(),
        status: 'ready',
        progress: 1,
        error: '',
        documentSource: native
            ? { kind: 'DOCUMENT', sessionId: 1, documentId: 'tx16w', expectedRevision: 1 }
            : undefined,
        destinationName: native
            ? query.get('scenario') === 'native-long'
                ? `${'LongDestinationImageName'.repeat(9)}.hds`
                : 'Target image.hds'
            : undefined,
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
                  origin: index === 1 ? ('AUTO_ADDED' as const) : ('SELECTED' as const),
                  sourceIdentity: `fixture:${index}`,
              })),
    });
    const jobs = {
        waitForJob: async () => ({ jobId: 1, kind: 'conversions.execute', status: 'completed' as const }),
        cancelJob: async () => undefined,
    };
    const completion = new ImportCompletion(jobs, new JobController(jobs));
    if (query.has('layout')) {
        diskLayoutCompletion(completion, scenario);
        plan.report!.capacity = diskLayoutCapacity();
        if (scenario === 'empty') {
            request.members = [];
            request.plan = null;
            request.status = 'waiting-target';
        }
        if (scenario === 'planning') {
            request.plan = null;
            request.status = 'inspecting';
        }
        if (scenario === 'expired') {
            request.plan = null;
            request.status = 'ready';
            request.error = 'Conversion plan expired. Prepare again.';
        }
        if (scenario === 'missing') {
            request.plan!.report!.executable = false;
            request.plan!.report!.issues.push({
                id: 'missing',
                code: 'MISSING_WAVE',
                impact: 'BLOCKER',
                message: 'Wave file PFD1 is missing. Add the companion disk.',
                sourceObject: 'PFD1',
                sourceParameter: '',
                targetObject: '',
                targetParameter: '',
            });
            request.plan!.report!.issueCount++;
        }
        if (scenario === 'strict') fidelity('STRICT');
        if (
            ['companion-found', 'searching', 'search-error', 'mixed-losses', 'all-losses', 'long-chips'].includes(
                scenario,
            )
        ) {
            request.members = request.members.slice(0, 2);
            request.members[0].sourceName =
                scenario === 'long-chips' ? `${'Long original library disk filename '.repeat(12)}.img` : 'sd001ye.img';
            request.members[1].sourceName = 'sd002ye.img';
        }
        if (scenario === 'searching') {
            request.plan = null;
            request.status = 'discovering';
            request.discovery = {
                state: 'searching',
                message: 'Looking for companion disks...',
                error: '',
                requirements: [],
            };
        }
        if (scenario === 'search-error') {
            request.discovery = {
                state: 'error',
                message: 'Companion search incomplete. Retry or add disks explicitly.',
                error: 'Directory scan exceeds 1024 candidates',
                requirements: [],
            };
        }
        if (['mixed-losses', 'all-losses'].includes(scenario)) {
            plan.report!.issues = [
                ...Array.from({ length: 101 }, (_, index) => ({
                    ...plan.report!.issues[0],
                    id: `loss-${index}`,
                    message: `Wave Data ${index} resampled to 48000 Hz`,
                })),
                plan.report!.issues[1],
            ];
            plan.report!.issueCount = 102;
            request.acknowledgedIssueIds = plan
                .report!.issues.slice(0, scenario === 'all-losses' ? 101 : 50)
                .map((issue) => issue.id);
        }
    }
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
        onprepare={() => {
            request.discovery = idleCompanionSearch();
            request.error = '';
            fidelity(request.fidelity);
        }}
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
