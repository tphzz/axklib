<script lang="ts">
    import type { Su700Workflow } from './su700Workflow.svelte';
    import { modal } from '../../../lib/modal';
    import Icon from '../../../lib/components/Icon.svelte';
    import ImportDestinationChooser from '../../../lib/components/ImportDestinationChooser.svelte';
    import FilesExportTree from '../../files/FilesExportTree.svelte';
    import { formatStoredSize } from '../../../lib/formatBytes';
    let { workflow }: { workflow: Su700Workflow } = $props();
    const preview = $derived({
        totalBytes: workflow.inspection?.totalBytes ?? 0,
        entries: (workflow.inspection?.files ?? [])
            .filter((file) => file.role !== 'EXTRA' || workflow.extras.includes(file.sourcePath))
            .map((file) => ({ relativePath: file.relativePath, directory: false, sizeBytes: file.sizeBytes })),
    });
</script>

{#if workflow.opened}
    <div class="dialog-backdrop" role="presentation">
        <div
            class="dialog-shell su700-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Import SU700 floppy"
            aria-busy={workflow.busy}
            use:modal={{ onescape: () => void workflow.close() }}
        >
            <header class="dialog-header">
                <h2>Import SU700 floppy</h2>
                <button
                    type="button"
                    class="icon-button"
                    aria-label="Close"
                    disabled={!workflow.canDismiss}
                    onclick={() => void workflow.close()}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="su700-content">
                <div class="summary">
                    <strong title={workflow.source?.displayName}
                        >{workflow.source?.displayName ?? 'SU700 floppy'}</strong
                    ><span
                        >{workflow.inspection?.songCount ?? 0}
                        {workflow.inspection?.songCount === 1 ? 'song' : 'songs'}, {workflow.inspection?.sampleCount ??
                            0} samples, {formatStoredSize(
                            preview.entries.reduce((total, file) => total + file.sizeBytes, 0),
                        )}</span
                    >
                </div>
                <ImportDestinationChooser
                    mode="create"
                    unavailableModes={{ existing: 'SU700 floppy import requires a new volume' }}
                    partitionIndex={workflow.rootIndex}
                    volumeName={workflow.name}
                    partitions={workflow.roots.map((root, index) => ({ partitionIndex: index, name: root.name }))}
                    volumes={[]}
                    disabled={workflow.busy ||
                        workflow.phase !== 'review' ||
                        workflow.inspection?.status !== 'COMPLETE'}
                    onmode={() => undefined}
                    onvolume={() => undefined}
                    onpartition={(index) => workflow.updateRoot(index)}
                    onname={(name) => workflow.updateName(name)}
                />
                <FilesExportTree inspection={preview} name={workflow.name || 'New volume'} label="Import entries" />
                {#if workflow.warnings.length}<div class="dialog-results dialog-warning" role="alert">
                        {#each workflow.warnings as warning}<p>{warning}</p>{/each}
                    </div>{/if}
                {#if workflow.inspection?.files.some((file) => file.role === 'EXTRA')}
                    <div class="extras" role="group" aria-label="Additional files">
                        {#each workflow.inspection.files.filter((file) => file.role === 'EXTRA') as file (file.sourcePath)}
                            <label
                                ><input
                                    class="dialog-checkbox"
                                    type="checkbox"
                                    checked={workflow.extras.includes(file.sourcePath)}
                                    disabled={workflow.busy || workflow.phase !== 'review'}
                                    onchange={() => workflow.toggleExtra(file.sourcePath)}
                                />{file.sourcePath}</label
                            >
                        {/each}
                    </div>
                {/if}
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status" title={workflow.message}>{workflow.message}</span>
                <button
                    class="secondary-button"
                    type="button"
                    disabled={!workflow.canDismiss}
                    onclick={() => void workflow.close()}
                    >{workflow.phase === 'warnings'
                        ? 'Done'
                        : workflow.phase === 'complete' || workflow.phase === 'refresh-failed'
                          ? 'Close'
                          : 'Cancel'}</button
                >
                {#if workflow.phase === 'unconfirmed'}<button
                        class="primary-button"
                        disabled={workflow.busy}
                        onclick={() => void workflow.checkStatus()}>Check status</button
                    >
                {:else if workflow.phase === 'refresh-failed'}<button
                        class="primary-button"
                        disabled={workflow.busy}
                        onclick={() => void workflow.retryRefresh()}>Refresh</button
                    >
                {:else if workflow.phase === 'review'}
                    <button
                        class="secondary-button"
                        type="button"
                        disabled={!workflow.canReview}
                        onclick={() => void workflow.review()}>Review</button
                    >
                    <button
                        class="primary-button"
                        type="button"
                        disabled={!workflow.canSubmit}
                        onclick={() => void workflow.submit()}>Import</button
                    >
                {/if}
            </footer>
        </div>
    </div>
{/if}

<style>
    .su700-dialog {
        width: min(740px, calc(100vw - 32px));
        height: min(560px, calc(100dvh - 32px));
        display: flex;
        flex-direction: column;
    }
    .su700-content {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
        gap: 12px;
        padding: 12px;
    }
    .summary {
        display: flex;
        gap: 8px;
        font-size: var(--dialog-body-font-size);
    }
    .summary strong {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .summary span {
        white-space: nowrap;
    }
    .extras {
        max-height: 64px;
        overflow: auto;
        scrollbar-gutter: stable;
        padding-right: calc(8px + var(--overlay-scrollbar-clearance));
        font-size: var(--dialog-body-font-size);
    }
    .extras label {
        display: flex;
        align-items: center;
        gap: 6px;
        min-height: 24px;
    }
</style>
