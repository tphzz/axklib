<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import ExportDestinationChooser from '../../lib/components/ExportDestinationChooser.svelte';
    import { formatStoredSize } from '../../lib/formatBytes';
    import type { FilesExportWorkflow } from './exportWorkflow.svelte';
    import FilesExportTree from './FilesExportTree.svelte';
    let { workflow }: { workflow: FilesExportWorkflow } = $props();
    const failed = $derived(workflow.phase === 'failed' || workflow.phase === 'unconfirmed');
    let noticePage = $state(0);
    let noticesElement: HTMLUListElement | undefined = $state();
    const noticePages = $derived(Math.max(1, Math.ceil((workflow.inspection?.notices.length ?? 0) / 100)));
    $effect(() => {
        if (noticePage >= noticePages) noticePage = noticePages - 1;
    });
    function changeNoticePage(page: number) {
        noticePage = page;
        if (noticesElement) noticesElement.scrollTop = 0;
    }
</script>

{#snippet pagination(label: string, page: number, pages: number, change: (page: number) => void)}
    {#if pages > 1}
        <nav class="export-pagination" aria-label={label}>
            <button
                class="icon-button previous-page"
                type="button"
                aria-label="Previous page"
                title="Previous page"
                disabled={page === 0}
                onclick={() => change(page - 1)}><Icon name="chevron" size={14} /></button
            >
            <span>{page + 1} / {pages}</span>
            <button
                class="icon-button"
                type="button"
                aria-label="Next page"
                title="Next page"
                disabled={page === pages - 1}
                onclick={() => change(page + 1)}><Icon name="chevron" size={14} /></button
            >
        </nav>
    {/if}
{/snippet}

{#if workflow.review && workflow.phase !== 'choosing'}
    <div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
        <div
            class="dialog-shell files-export-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Export files"
            aria-busy={workflow.busy}
            use:modal={{ onescape: () => workflow.close() }}
        >
            <header class="dialog-header">
                <h2>Export files</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={workflow.busy && workflow.phase !== 'inspecting'}
                    onclick={() => workflow.close()}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="files-export-content">
                <div class="export-summary">
                    <strong title={workflow.review.name}>{workflow.review.name}</strong>
                    <span>{workflow.inspection ? `${workflow.entryCount} entries` : 'Inspecting'}</span>
                    <span>{workflow.inspection ? formatStoredSize(workflow.inspection.totalBytes) : ''}</span>
                </div>
                {#if workflow.inspection}
                    {#key workflow.inspection}
                        <FilesExportTree
                            inspection={workflow.inspection}
                            name={workflow.destinationName || workflow.inspection.rootDirectory?.name || 'Files'}
                        />
                    {/key}
                {:else}<div class="export-placeholder"></div>{/if}
                {#if workflow.inspection?.notices.length}
                    <ul class="export-notices dialog-warning" aria-label="Export notices" bind:this={noticesElement}>
                        {#each workflow.inspection.notices.slice(noticePage * 100, (noticePage + 1) * 100) as notice}
                            <li>{notice.sourcePath}: {notice.message}</li>
                        {/each}
                    </ul>
                    {@render pagination('Export notice pages', noticePage, noticePages, changeNoticePage)}
                {/if}
                {#if !workflow.review.directComputer && workflow.route === null}
                    <ExportDestinationChooser
                        desktop={workflow.review.desktop}
                        disabled={!workflow.canChoose}
                        onworkspace={() => void workflow.choose('workspace')}
                        onlocal={() => void workflow.choose('computer')}
                    />
                {/if}
            </div>
            <footer class="dialog-footer">
                <span
                    class="export-status"
                    class:dialog-error={failed}
                    title={workflow.message}
                    role={failed ? 'alert' : 'status'}>{workflow.message}</span
                >
                <button
                    type="button"
                    class="secondary-button"
                    data-dialog-initial-focus="caret"
                    disabled={workflow.busy && workflow.phase !== 'inspecting' && !workflow.canCancel}
                    onclick={() => (workflow.canCancel ? void workflow.cancel() : workflow.close())}
                >
                    {workflow.phase === 'completed'
                        ? 'Done'
                        : workflow.phase === 'ready' || workflow.busy
                          ? 'Cancel'
                          : 'Close'}
                </button>
                {#if workflow.phase === 'unconfirmed'}
                    <button
                        type="button"
                        class="primary-button"
                        disabled={workflow.jobId === null}
                        onclick={() => void workflow.checkStatus()}>Check status</button
                    >
                {:else if workflow.phase !== 'completed' && (workflow.review.directComputer || workflow.route !== null)}
                    <button
                        type="button"
                        class="primary-button"
                        disabled={!workflow.canChoose}
                        onclick={() => void workflow.choose(workflow.route ?? 'computer')}>Export...</button
                    >
                {/if}
            </footer>
        </div>
    </div>
{/if}

<style>
    .export-pagination {
        display: flex;
        justify-content: flex-end;
        align-items: center;
        gap: 8px;
        flex-shrink: 0;
        font-size: var(--dialog-metadata-font-size);
    }
    .previous-page :global(svg) {
        transform: rotate(180deg);
    }
    .files-export-dialog {
        width: min(640px, calc(100vw - 32px));
        height: min(520px, calc(100dvh - 32px));
        display: flex;
        flex-direction: column;
    }
    .files-export-content {
        display: flex;
        flex: 1;
        min-height: 0;
        flex-direction: column;
        gap: 12px;
        padding: 12px;
    }
    .export-summary {
        display: flex;
        gap: 12px;
        align-items: center;
        font-size: var(--dialog-body-font-size);
    }
    .export-summary strong {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .export-placeholder {
        flex: 1;
        min-height: 0;
    }
    .export-notices {
        margin: 0;
        padding: 0 calc(8px + var(--overlay-scrollbar-clearance)) 0 0;
        list-style: none;
        max-height: 72px;
        overflow: auto;
        scrollbar-gutter: stable;
        overflow-wrap: anywhere;
        font-size: var(--dialog-metadata-font-size);
    }
    .export-status {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        font-size: var(--dialog-metadata-font-size);
    }
</style>
