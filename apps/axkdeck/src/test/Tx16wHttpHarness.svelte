<script lang="ts">
    import { onDestroy } from 'svelte';
    import { HttpImageTransport } from '../lib/httpTransport';
    import type { AxklibApiConnection } from '../lib/httpApiClient';
    import type { OpenedImage } from '../lib/transport';
    import { serverFileLocation } from '../lib/storageLocations';
    import { JobController } from '../features/jobs/actions';
    import { Tx16wImportWorkflow } from '../features/import/tx16wWorkflow.svelte';
    import Tx16wImportDialog from '../lib/components/Tx16wImportDialog.svelte';
    import { ASeriesPreferences, provideASeriesPreferences } from '../lib/aSeriesPreferences.svelte';

    const connection = (window as Window & { tx16wConnection?: AxklibApiConnection }).tx16wConnection;
    if (!connection) throw new Error('The HTTP acceptance runner must provide its isolated test connection');
    const transport = new HttpImageTransport({ ...connection, mode: 'remote' });
    const jobs = new JobController(transport);
    const preferences = new ASeriesPreferences({ load: async () => 'A4000_A5000', save: async () => {} });
    provideASeriesPreferences(preferences);
    let opened = $state<OpenedImage | null>(null);
    let status = $state('Ready');
    const destination = serverFileLocation({ rootId: 'target', relativePath: 'target.hds' });
    const target = { partitionIndex: 0, volumeName: 'test05' };
    const workflow = new Tx16wImportWorkflow({
        transport,
        jobs,
        sessionId: () => opened?.sessionId ?? null,
        imageLocation: () => (opened ? destination : null),
        mutationsAvailable: () => opened?.volumeMutationsAvailable ?? false,
        selectedSource: () => ({
            id: 'target',
            kind: 'volume',
            name: target.volumeName,
            partitionIndex: 0,
            childCount: 0,
        }),
        sourceItems: () => opened?.tree ?? [],
        refreshSession: async () => {
            opened = await transport.refreshImage(opened!.sessionId);
        },
        invalidateSession: async () => {},
        selectWorkspace: () => {},
        setStatus: (value) => {
            status = value;
        },
        reportTiming: () => {},
    });
    workflow.completion.capacity.target = 'A4000_A5000';
    Object.assign(window, {
        tx16wAcceptance: {
            begin: async (rootId: string, names: string[]) => {
                await workflow.close();
                if (!opened) opened = await transport.openImage([destination]);
                await workflow.requestDroppedFiles(
                    names.map((relativePath) => serverFileLocation({ rootId, relativePath })),
                    target,
                );
            },
            snapshot: () => ({
                status,
                phase: workflow.completion.phase,
                request: workflow.request
                    ? {
                          status: workflow.request.status,
                          error: workflow.request.error,
                          members: workflow.request.members.map((member) => ({
                              name: member.sourceName,
                              origin: member.origin,
                          })),
                          discovery: workflow.request.discovery,
                          report: workflow.request.plan?.report
                              ? {
                                    executable: workflow.request.plan.report.executable,
                                    actionCount: workflow.request.plan.report.actionCount,
                                    issueCount: workflow.request.plan.report.issueCount,
                                    counts: Object.fromEntries(
                                        ['BLOCKER', 'LOSS', 'UNCERTAINTY', 'REPRESENTATION'].map((impact) => [
                                            impact,
                                            workflow.request!.plan!.report!.issues.filter(
                                                (issue) => issue.impact === impact,
                                            ).length,
                                        ]),
                                    ),
                                }
                              : null,
                      }
                    : null,
            }),
            close: () => workflow.close(),
            shutdown: async () => {
                await workflow.dispose();
                await jobs.dispose();
                if (opened) await transport.closeImage(opened.sessionId);
                opened = null;
            },
        },
    });
    onDestroy(() => {
        delete (window as Window & { tx16wAcceptance?: unknown }).tx16wAcceptance;
    });
</script>

{#if workflow.request}
    <Tx16wImportDialog
        request={workflow.request}
        completion={workflow.completion}
        volumeOptions={workflow.volumeOptions()}
        onrecover={() => void workflow.recoverCompletion()}
        ontarget={(value) => void workflow.selectTarget(value)}
        onmode={(value) => void workflow.selectImportMode(value)}
        onfidelity={(value) => void workflow.selectFidelity(value)}
        onprepare={() => void workflow.reprepare()}
        onadd={(files) => void workflow.addFiles(files)}
        onremove={(id) => workflow.removeMember(id)}
        onconfirm={() => void workflow.commit()}
        oncancel={() => void workflow.close()}
    />
{:else}<p role="status">{status}</p>{/if}
