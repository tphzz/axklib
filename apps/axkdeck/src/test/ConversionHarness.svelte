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
    import { placementError } from '../features/import/tx16wPlacement';

    const target = { kind: 'EXISTING_VOLUME' as const, partitionIndex: 0, volumeName: 'Samples' };
    const destinations = {
        partitions: [
            { partitionIndex: 0, name: 'PARTITION 1' },
            { partitionIndex: 1, name: 'PARTITION 2' },
        ],
        volumes: [{ ...target, name: 'PARTITION 1', label: 'Samples' }],
    };
    const query = new URLSearchParams(location.search);
    const scenario = query.get('scenario') ?? 'normal';
    const typhoon = scenario === 'typhoon-import' || scenario === 'typhoon-missing';
    const native = query.has('native') || scenario === 'native-long' || scenario === 'typhoon-missing';
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
            ...(native || typhoon
                ? {
                      sourceCount: 4,
                      sourceObjects: [
                          {
                              id: 'p1',
                              name: 'Stage Piano',
                              kind: 'performance',
                              location: 'Keys-01.img / P01',
                              selectable: true,
                              dependencies: ['v1'],
                          },
                          {
                              id: 'p2',
                              name: 'EP Layer',
                              kind: 'performance',
                              location: 'Keys-02.img / P02',
                              selectable: true,
                              dependencies: ['w1'],
                          },
                          {
                              id: 'v1',
                              name: 'Piano Voice',
                              kind: 'voice',
                              location: 'Keys-01.img / V01',
                              selectable: false,
                              dependencies: ['w1'],
                          },
                          {
                              id: 'w1',
                              name: 'PFC2',
                              kind: 'wave',
                              location: 'Keys-02.img / W01',
                              selectable: false,
                              dependencies: [],
                          },
                      ],
                  }
                : {}),
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
                    sourceObject: native ? 'w1' : 'native-wave-1',
                    sourceParameter: 'sample_rate',
                    targetObject: 'Concert-Piano-1',
                    targetParameter: 'sample_rate',
                },
                {
                    id: 'default',
                    code: 'defaults',
                    impact: 'UNCERTAINTY',
                    message: 'Destination controls use writer defaults',
                    sourceObject: native ? 'w1' : 'native-wave-1',
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
        sourceName: native ? 'Keys-01.img' : undefined,
        destinationProfile: native ? 'yamaha.tx16w-to-a-series' : undefined,
        targetLabel: native ? 'A-series · Hard disk (SFS)' : undefined,
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
    const missingVersion = {
        id: 'missing-version',
        code: 'typhoon.unresolved-reference',
        impact: 'BLOCKER' as const,
        sourceObject: 'v2',
        sourceParameter: '',
        targetObject: '',
        targetParameter: '',
        message:
            'Strings needs Wave STR-HI (version 00000042) from STRINGS2. This version is missing. Required by: Late Strings. Add the disk containing this version or remove the affected instruments from the selection.',
    };
    if (typhoon) {
        request.sourceFormat = 'nuedge.typhoon';
        request.destinationProfile = plan.report!.profileId = 'nuedge.typhoon-to-a-series';
        const report = request.plan!.report!;
        report.sourceObjects = [
            {
                id: 'p1',
                name: 'Stage Piano',
                kind: 'performance',
                location: 'Keys-A.img / STAGE.P01',
                selectable: true,
                dependencies: ['v1'],
            },
            {
                id: 'p2',
                name: 'EP Layer',
                kind: 'performance',
                location: 'Keys-A.img / EP.P01',
                selectable: true,
                dependencies: ['v1'],
            },
            {
                id: 'p3',
                name: 'Late Strings',
                kind: 'performance',
                location: 'Keys-B.img / LATE.P01',
                selectable: true,
                dependencies: ['v2'],
            },
            {
                id: 'v1',
                name: 'Piano',
                kind: 'voice',
                location: 'Keys-A.img / PIANO.O01',
                selectable: false,
                dependencies: ['w1'],
            },
            {
                id: 'v2',
                name: 'Strings',
                kind: 'voice',
                location: 'Keys-B.img / STRINGS.O01',
                selectable: false,
                dependencies: ['w2'],
            },
            {
                id: 'w1',
                name: 'PF-C2',
                kind: 'wave',
                location: 'Keys-A.img / PF-C2.C01',
                selectable: false,
                dependencies: [],
            },
            {
                id: 'w2',
                name: 'STR-LO',
                kind: 'wave',
                location: 'Keys-B.img / STR-LO.C01',
                selectable: false,
                dependencies: [],
            },
        ];
        report.sourceCount = report.sourceObjects.length;
        report.actions = [
            { kind: 'wave-data', name: 'PF-C2', sourceObject: 'w1' },
            { kind: 'sample', name: 'PF-C2', sourceObject: 'v1' },
            { kind: 'sample-bank', name: 'Piano', sourceObject: 'v1' },
            { kind: 'program', name: 'Stage Piano', sourceObject: 'p1' },
        ].map((row) => ({ ...row, action: 'create', details: [] }));
        report.actionCount = report.actions.length;
        report.issues = [];
        report.issueCount = 0;
        if (scenario === 'typhoon-import') {
            request.members = request.members.slice(0, 2);
            request.members[0].sourceName = 'Keys-A.img';
            request.members[1].sourceName = 'Keys-B.img';
            request.sourceObjectIds = ['p1'];
            request.target = { kind: 'CREATE_VOLUME', partitionIndex: 0, volumeName: 'Typhoon Keys' };
        } else {
            request.sourceName = 'Keys-A.img + Keys-B.img';
            request.sourceObjectIds = ['p1', 'p2', 'p3'];
            report.issues = [missingVersion];
            report.issueCount = 1;
            report.executable = false;
        }
    }
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
            [
                'companion-found',
                'searching',
                'search-error',
                'conversion-notes',
                'audition-program',
                'long-chips',
            ].includes(scenario)
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
        if (['conversion-notes', 'audition-program'].includes(scenario)) {
            plan.report!.issues = [
                ...Array.from({ length: 101 }, (_, index) => ({
                    ...plan.report!.issues[0],
                    id: `loss-${index}`,
                    message: `Wave Data ${index} resampled to 48000 Hz`,
                })),
                plan.report!.issues[1],
            ];
            plan.report!.issueCount = 102;
            if (scenario === 'audition-program') {
                plan.report!.issues.push({
                    ...plan.report!.issues[101],
                    id: 'audition-program',
                    code: 'tx16w.missing_performance',
                    message: 'No TX16W Performance was present',
                    decision: 'Create an audition Program from Piano Voice',
                });
                plan.report!.issueCount++;
            }
        }
    }
    function fidelity(value: 'BEST' | 'STRICT') {
        request.fidelity = value;
        request.acknowledgedIssueIds = [];
        if (typhoon && request.plan?.report) {
            request.plan.report.fidelity = value;
            return;
        }
        request.plan = { ...plan, report: { ...plan.report!, fidelity: value, executable: value === 'BEST' } };
    }
</script>

{#if open}
    <Tx16wImportDialog
        {request}
        {completion}
        onbrowse={() => {
            request.destinationName = 'Second target.hds';
        }}
        onsources={(ids) => {
            request.sourceObjectIds = ids;
            if (scenario === 'typhoon-missing' && request.plan?.report) {
                const report = request.plan.report;
                report.issues = !ids || ids.includes('p3') ? [missingVersion] : [];
                report.issueCount = report.issues.length;
                report.executable = !!ids?.length && report.issueCount === 0;
            }
        }}
        {destinations}
        onrecover={() => {
            completion.phase = 'completed';
            open = false;
        }}
        ontarget={(value) => {
            request.target = value;
            request.sourceObjects = plan.report?.sourceObjects;
            request.plan = placementError(value, destinations) ? null : plan;
            request.status = request.plan ? 'ready' : 'waiting-target';
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
