<script lang="ts">
    import { onMount } from 'svelte';
    import {
        ASeriesPreferences,
        provideASeriesPreferences,
        type ASeriesGeneration,
    } from '../lib/aSeriesPreferences.svelte';
    import { HttpCapacityGate } from '../lib/httpCapacityGate';
    import type { AxklibHttpApiClient } from '../lib/httpApiClient';
    import type { HttpJobController } from '../lib/httpJobController';
    import { volumeMutationOperation } from '../lib/httpTransportWire';
    import type { ImageTransport, JobState, VolumeMutation } from '../lib/transport';
    import { JobController } from '../features/jobs/actions';
    import { MutationWorkflow } from '../features/mutation/workflow.svelte';
    import { MutationCapacityWorkflow } from '../features/mutation/capacityWorkflow.svelte';
    import MutationCapacityDialog from '../lib/components/MutationCapacityDialog.svelte';
    import VolumeActionDialog from '../lib/components/VolumeActionDialog.svelte';
    import { modal } from '../lib/modal';
    import { volumeCapacityFixture } from './volumeCapacityFixture';

    let mode = $state('EMPTY');
    let target = $state<ASeriesGeneration>('A4000_A5000');
    let revision = $state(1);
    let exists = $state(true);
    let checks = $state(0);
    let writes = $state(0);
    let refreshes = $state(0);
    let selected = $state('bar');
    let lastTarget = $state('');
    let utility = $state(false);
    let ready = $state(false);
    const completedJobs = new Set<number>();
    const lostJobs = new Set<number>();
    let refreshFailed = false;
    const partition = { id: 'p', kind: 'partition' as const, name: 'PARTITION 1', childCount: 1, partitionIndex: 0 };
    const volume = { ...partition, id: 'bar', kind: 'volume' as const, name: 'bar' };
    const preferences = new ASeriesPreferences({ load: async () => target, save: async () => {} });
    provideASeriesPreferences(preferences);
    const capacity = new MutationCapacityWorkflow(preferences);
    let pending: VolumeMutation[] = [];
    const gate = new HttpCapacityGate(
        {
            invoke: async (operation: string, request: { capacityPolicy: { target: string } }) => {
                if (operation === 'images.alter.inspect') {
                    checks++;
                    lastTarget = request.capacityPolicy.target;
                    await new Promise((resolve) => setTimeout(resolve, 80));
                    if (mode === 'ERROR') throw new Error('Capacity service disconnected');
                    return {
                        capacity: {
                            target: request.capacityPolicy.target,
                            allowed: mode !== 'DENIED',
                            reports:
                                pending[0]?.kind === 'delete'
                                    ? []
                                    : [
                                          {
                                              ...volumeCapacityFixture(mode === 'DENIED' ? 'DOES_NOT_FIT' : 'FITS'),
                                              volumeName: 'bar',
                                              objectCounts: mode === 'POPULATED' ? [{ type: 'SBNK', count: 40 }] : [],
                                          },
                                      ],
                        },
                    };
                }
                writes++;
                return { jobId: writes, status: 'queued' };
            },
        } as unknown as AxklibHttpApiClient,
        {
            isJob: (value: { jobId?: number }) => typeof value?.jobId === 'number',
            map: (value: JobState) => value,
        } as unknown as HttpJobController,
        (check, context) => capacity.review(check, context),
    );
    const transport = {
        inspectVolumeDeletion: async () => ({
            imageId: 'fixture',
            revision,
            targets: [{ partitionIndex: 0, volumeName: 'bar' }],
            canDelete: true,
            crossingRelationshipCount: 0,
            blockers: [],
        }),
        startVolumeMutations: async (_session: number, mutations: VolumeMutation[]) => {
            pending = mutations;
            return gate.start({
                imageId: 'fixture',
                expectedRevision: revision,
                inputBindings: [],
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: mutations.map((mutation) => volumeMutationOperation(mutation)),
                    },
                },
            });
        },
        waitForJob: async (jobId: number): Promise<JobState> => {
            await new Promise((resolve) => setTimeout(resolve, 80));
            if (!completedJobs.has(jobId)) {
                completedJobs.add(jobId);
                exists = pending[0]?.kind === 'add';
                revision++;
            }
            if (mode === 'LOST_WAIT' && !lostJobs.has(jobId)) {
                lostJobs.add(jobId);
                throw new Error('Job connection lost');
            }
            return { jobId, status: 'completed', kind: 'fixture' };
        },
        cancelJob: async () => {},
    } as unknown as ImageTransport;
    const workflow = new MutationWorkflow({
        transport,
        jobs: new JobController(transport),
        catalog: {} as never,
        audition: { invalidateSession: async () => {} } as never,
        sessionId: () => 1,
        imageOpen: () => true,
        workspaceView: () => 'samples',
        setWorkspaceView: () => {},
        clearSelection: () => {},
        refreshSession: async (preferred) => {
            refreshes++;
            if (mode === 'REFRESH_ERROR' && !refreshFailed) {
                refreshFailed = true;
                throw new Error('Workspace refresh disconnected');
            }
            selected = preferred?.volumeName ?? '';
        },
        setStatus: () => {},
        reportTiming: () => {},
    });
    workflow.setCapabilities({
        volumeMutationsAvailable: true,
        partitionMutationsAvailable: true,
        objectRenameAvailable: false,
    });
    onMount(() => {
        void preferences.ready.then(() => {
            ready = true;
        });
        return () => capacity.dispose();
    });
</script>

{#if ready}
    <nav>
        <button class="secondary-button" onclick={() => workflow.requestVolumeAction(partition, 'add-volume')}
            >New volume</button
        >
        <button
            class="secondary-button"
            disabled={!exists}
            onclick={() => workflow.requestVolumeAction(volume, 'delete-volume')}>Delete bar</button
        >
        <select class="dialog-field-control" aria-label="Scenario" bind:value={mode}>
            <option value="EMPTY">Empty</option><option value="POPULATED">Populated</option><option value="DENIED"
                >Denied</option
            ><option value="ERROR">Error</option>
            <option value="LOST_WAIT">Lost job connection</option><option value="REFRESH_ERROR">Refresh error</option>
        </select>
        <select
            class="dialog-field-control"
            aria-label="Preferred A-Series generation"
            bind:value={target}
            onchange={() => void preferences.save(target)}
        >
            <option value="A3000">a3k</option><option value="A4000_A5000">a4k/a5k</option>
        </select>
    </nav>
    <section class="background-pane" data-background-pane>
        {#each Array.from({ length: 100 }, (_, index) => index) as index}<p>Volume row {index + 1}</p>{/each}
    </section>
    <output data-volume-state
        >{JSON.stringify({
            revision,
            exists,
            checks,
            writes,
            refreshes,
            selected,
            lastTarget,
            phase: workflow.volumeActionPhase,
        })}</output
    >
    <!-- Match the application's mount order: capacity review precedes the raised parent. -->
    <MutationCapacityDialog workflow={capacity} />
    {#if workflow.volumeAction}
        <VolumeActionDialog
            action={workflow.volumeAction.action}
            items={workflow.volumeAction.items}
            busy={workflow.volumeActionBusy}
            phase={workflow.volumeActionPhase}
            locked={workflow.volumeActionLocked}
            canDismiss={workflow.volumeActionCanDismiss}
            recovery={workflow.volumeActionRecovery}
            onrecover={() => void workflow.recoverVolumeAction()}
            error={workflow.volumeActionError}
            deletionInspection={workflow.volumeDeletionInspection}
            oncancel={() => workflow.cancelVolumeAction()}
            onsubmit={(name) => void workflow.submitVolumeAction(name)}
        />
    {/if}
    {#if capacity.request}
        <button class="test-utility" onclick={() => (utility = true)}>Nested utility</button>
    {/if}
    {#if utility}
        <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
            <div
                class="dialog-shell"
                role="dialog"
                aria-modal="true"
                aria-label="Nested utility"
                use:modal={{ onescape: () => (utility = false) }}
            >
                <header class="dialog-header"><h2>Nested utility</h2></header>
                <div class="utility-pane" data-utility-pane>
                    {#each Array.from({ length: 40 }, (_, index) => index) as index}<p>
                            Utility row {index + 1}
                        </p>{/each}
                </div>
                <footer class="dialog-footer">
                    <button class="secondary-button" onclick={() => (utility = false)}>Close</button>
                </footer>
            </div>
        </div>
    {/if}
{/if}

<style>
    :global(body) {
        margin: 0;
        background: var(--color-bg);
    }
    nav {
        display: flex;
        gap: 8px;
        padding: 12px;
        flex-wrap: wrap;
    }
    .background-pane {
        height: 460px;
        overflow: auto;
    }
    .background-pane p {
        margin: 0;
        padding: 8px;
        border-bottom: 1px solid var(--color-border);
    }
    output {
        display: block;
        padding: 12px;
        overflow-wrap: anywhere;
    }
    .utility-pane {
        max-height: 180px;
        overflow: auto;
        padding: 12px;
    }
    .test-utility {
        display: none;
    }
</style>
