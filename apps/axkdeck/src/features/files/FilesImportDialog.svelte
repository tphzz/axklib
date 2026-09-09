<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import ImportSourceChoice from '../../lib/components/ImportSourceChoice.svelte';
    import { browserUploadSource } from '../../lib/clientUploadSource';
    import { formatStoredSize } from '../../lib/formatBytes';
    import type { FilesImportWorkflow } from './importWorkflow.svelte';
    import { importPaths } from './importReview';
    let { workflow }: { workflow: FilesImportWorkflow } = $props();
    let input = $state<HTMLInputElement>();
    function dismiss() {
        if (!workflow.canDismiss) return;
        if (workflow.phase === 'writing') void workflow.cancel();
        else workflow.close();
    }
    let page = $state(0);
    const pageCount = $derived(Math.max(1, Math.ceil(workflow.rows.length / 100)));
    const paths = $derived(importPaths(workflow.rows));
    const hierarchy = $derived(workflow.rows.some((row) => row.directory));
    const failed = $derived(['failed', 'unconfirmed', 'refresh-failed', 'write-failed'].includes(workflow.phase));
    $effect(() => {
        if (page >= pageCount) page = pageCount - 1;
    });
    const outcome = {
        CREATE_FILE: 'Create',
        SKIP_FILE: 'Skip',
        REPLACE_FILE: 'Replace',
        CONFLICT: 'Conflict',
        CREATE_DIRECTORY: 'Create',
        MERGE_DIRECTORY: 'Merge',
    };
    function localFiles(): void {
        if (!input) return;
        const files = Array.from(input.files ?? []).map(browserUploadSource);
        input.value = '';
        void workflow.chooseLocal(files);
    }
</script>

{#if workflow.target && workflow.phase !== 'picking'}
    <div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
        <div
            class="dialog-shell files-import-dialog"
            role="dialog"
            aria-modal="true"
            aria-label={workflow.directoryMode ? 'Import from disk' : 'Add files'}
            aria-busy={workflow.busy}
            use:modal={{ onescape: dismiss }}
        >
            <header class="dialog-header">
                <h2>{workflow.directoryMode ? 'Import from disk' : 'Add files'}</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={!workflow.canDismiss}
                    onclick={dismiss}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="files-import-content">
                <div class="import-summary">
                    <strong title={workflow.target.path || workflow.target.name}
                        >{workflow.target.path || workflow.target.name}</strong
                    >
                    <span>{workflow.rows.length} {hierarchy ? 'entries' : 'files'}</span>
                </div>
                <input type="file" multiple hidden bind:this={input} onchange={localFiles} aria-label="Local files" />
                {#if !workflow.rows.length && workflow.editable}
                    <ImportSourceChoice
                        label="File sources"
                        heading="Source"
                        description=""
                        workspaceDetail="Configured folders"
                        computerDetail="Local files"
                        computerAvailable={workflow.supportsClientUploads}
                        onchooseworkspace={() => void workflow.chooseWorkspace()}
                        onchooselocal={() => input?.click()}
                    />
                    {#if workflow.capabilities?.createDirectory}<button
                            class="secondary-button"
                            type="button"
                            onclick={() => void workflow.chooseDirectory()}>Choose folder...</button
                        >{/if}
                {:else}
                    <div class="import-toolbar">
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={!workflow.editable}
                            onclick={() => void workflow.chooseWorkspace()}>Choose files...</button
                        >
                        {#if workflow.capabilities?.createDirectory}<button
                                class="secondary-button"
                                type="button"
                                disabled={!workflow.editable}
                                onclick={() => void workflow.chooseDirectory()}>Choose folder...</button
                            >{/if}
                        {#if workflow.supportsClientUploads}<button
                                class="secondary-button"
                                type="button"
                                disabled={!workflow.editable}
                                onclick={() => input?.click()}>This computer...</button
                            >{/if}
                        <span class="toolbar-spacer"></span>
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={!workflow.editable}
                            onclick={() => workflow.setAllConflicts('SKIP')}>Skip all</button
                        >
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={!workflow.editable}
                            onclick={() => workflow.setAllConflicts('REPLACE')}>Replace all</button
                        >
                    </div>
                {/if}
                <div class="import-table" role="table" aria-label="Import entries">
                    <div class="import-row import-heading" role="row">
                        <span role="columnheader">Filename</span><span role="columnheader">Size</span>
                        <span role="columnheader">If exists</span><span role="columnheader">Result</span>
                    </div>
                    <div class="import-rows" role="rowgroup">
                        {#each workflow.rows.slice(page * 100, (page + 1) * 100) as row, offset}
                            {@const index = page * 100 + offset}
                            <div class="import-row" role="row">
                                <span role="cell" class="import-name"
                                    ><input
                                        class="dialog-field-control"
                                        aria-label={`Filename ${index + 1}`}
                                        aria-invalid={!workflow.validName(row.name)}
                                        title={row.source?.displayName ?? paths[index].join('/')}
                                        value={row.name}
                                        disabled={!workflow.editable}
                                        autocomplete="off"
                                        oninput={(event) => workflow.rename(index, event.currentTarget.value)}
                                    />{#if hierarchy}<span
                                            class="import-parent"
                                            title={paths[index].slice(0, -1).join('/') || '/'}
                                        >
                                            <Icon name={row.directory ? 'folder' : 'file'} size={12} />
                                            <span>{paths[index].slice(0, -1).join('/') || '/'}</span>
                                        </span>{/if}</span
                                >
                                <span class="import-size" role="cell"
                                    >{row.snapshot ? formatStoredSize(row.snapshot.sizeBytes) : '-'}</span
                                >
                                <span role="cell"
                                    >{#if row.directory}<span class="import-result">Merge</span>{:else}<select
                                            class="dialog-field-control"
                                            aria-label={`If file exists ${index + 1}`}
                                            value={row.conflict}
                                            disabled={!workflow.editable}
                                            onchange={(event) =>
                                                workflow.setConflict(
                                                    index,
                                                    event.currentTarget.value === 'REPLACE' ? 'REPLACE' : 'SKIP',
                                                )}
                                        >
                                            <option value="SKIP">Skip</option><option value="REPLACE">Replace</option>
                                        </select>{/if}</span
                                >
                                <span
                                    class="import-result"
                                    class:dialog-error={row.decision?.action === 'CONFLICT'}
                                    role="cell"
                                    title={row.decision?.issue || ''}
                                    >{row.decision ? outcome[row.decision.action] : 'Not reviewed'}</span
                                >
                            </div>
                        {/each}
                    </div>
                </div>
                {#if pageCount > 1}
                    <nav class="import-pagination" aria-label="Import review pages">
                        <button
                            class="icon-button previous-page"
                            type="button"
                            aria-label="Previous page"
                            title="Previous page"
                            disabled={page === 0}
                            onclick={() => (page -= 1)}><Icon name="chevron" size={14} /></button
                        >
                        <span>{page + 1} / {pageCount}</span>
                        <button
                            class="icon-button"
                            type="button"
                            aria-label="Next page"
                            title="Next page"
                            disabled={page + 1 >= pageCount}
                            onclick={() => (page += 1)}><Icon name="chevron" size={14} /></button
                        >
                    </nav>
                {/if}
                {#if workflow.rows.some((row) => row.decision?.issue)}
                    <ul class="import-issues" aria-label="Import conflicts">
                        {#each workflow.rows.filter((row) => row.decision?.issue) as row}
                            <li class:dialog-error={row.decision?.action === 'CONFLICT'}>
                                {row.name}: {row.decision?.issue}
                            </li>
                        {/each}
                    </ul>
                {/if}
                <p class="import-name-hint">{workflow.capabilities?.nameHint}</p>
                {#if workflow.directoryMode}<p class="import-name-hint">
                        {workflow.sourceKind === 'drop'
                            ? 'Includes dropped folders and their contents.'
                            : 'Folder contents only. Symbolic links and special files are excluded.'}
                    </p>{/if}
                <p class="dialog-warning">
                    Raw filesystem changes can break sampler relationships. Relationships are not repaired.
                </p>
                {#if workflow.warnings.length}<div class="dialog-results dialog-warning" role="alert">
                        {#each workflow.warnings as warning}<p>{warning}</p>{/each}
                    </div>{/if}
            </div>
            <footer class="dialog-footer">
                <span
                    class="dialog-footer-status"
                    class:dialog-error={failed}
                    role={failed ? 'alert' : 'status'}
                    title={workflow.message}>{workflow.message}</span
                >
                <button
                    class="secondary-button"
                    type="button"
                    data-dialog-initial-focus="caret"
                    disabled={!workflow.canDismiss}
                    onclick={dismiss}
                >
                    {workflow.phase === 'warnings'
                        ? 'Done'
                        : workflow.busy || ['choosing', 'ready', 'dirty', 'unconfirmed'].includes(workflow.phase)
                          ? 'Cancel'
                          : 'Close'}</button
                >
                {#if ['unconfirmed', 'checking', 'refresh-failed', 'refreshing', 'write-failed'].includes(workflow.phase)}
                    <button
                        class="primary-button"
                        type="button"
                        disabled={workflow.busy || (workflow.phase === 'unconfirmed' && workflow.jobId === null)}
                        onclick={() => void workflow.submit()}
                        >{['unconfirmed', 'checking'].includes(workflow.phase) ? 'Check status' : 'Refresh'}</button
                    >
                {:else if workflow.phase !== 'warnings'}
                    <button
                        class="secondary-button"
                        type="button"
                        disabled={!workflow.canInspect}
                        onclick={() => void workflow.inspect()}>Review</button
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
    .files-import-dialog {
        width: min(740px, calc(100vw - 32px));
        height: min(560px, calc(100dvh - 32px));
        display: flex;
        flex-direction: column;
    }
    .files-import-content {
        display: flex;
        flex: 1;
        min-height: 0;
        flex-direction: column;
        gap: 12px;
        padding: 12px;
    }
    .import-summary,
    .import-toolbar {
        display: flex;
        gap: 8px;
        align-items: center;
        font-size: var(--dialog-body-font-size);
    }
    .import-name {
        display: flex;
        flex-direction: column;
        min-width: 0;
    }
    .import-parent {
        display: flex;
        align-items: center;
        gap: 4px;
        height: 16px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .import-parent span {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .import-summary strong,
    .toolbar-spacer {
        flex: 1;
        min-width: 0;
    }
    .import-summary strong,
    .import-result {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .import-table {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }
    .import-row {
        display: grid;
        grid-template-columns: minmax(100px, 1fr) 70px 100px 80px;
        gap: 8px;
        align-items: start;
        min-height: 30px;
        padding-block: 2px;
        border-bottom: 1px solid var(--color-border);
        font-size: var(--dialog-body-font-size);
    }
    .import-row > * {
        min-width: 0;
    }
    .import-row input,
    .import-row select {
        width: 100%;
    }
    .import-heading {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        min-height: 20px;
    }
    .import-heading,
    .import-rows {
        padding-right: calc(8px + var(--overlay-scrollbar-clearance));
        scrollbar-gutter: stable;
    }
    .import-heading {
        overflow-y: hidden;
    }
    .import-rows {
        flex: 1;
        min-height: 0;
        overflow: auto;
    }
    .import-size {
        text-align: right;
        padding-top: 5px;
    }
    .import-result {
        padding-top: 5px;
        display: block;
    }
    .import-name-hint,
    .import-issues {
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
        margin: 0;
    }
    .import-issues {
        max-height: 64px;
        overflow: auto;
        padding: 0 calc(8px + var(--overlay-scrollbar-clearance)) 0 0;
        list-style: none;
        scrollbar-gutter: stable;
        overflow-wrap: anywhere;
    }
    .dialog-warning {
        margin: 0;
        font-size: var(--dialog-body-font-size);
    }
    .import-pagination {
        display: flex;
        justify-content: flex-end;
        align-items: center;
        gap: 8px;
        font-size: var(--dialog-metadata-font-size);
    }
    .previous-page {
        transform: rotate(180deg);
    }
    @media (max-width: 600px) {
        .import-toolbar {
            flex-wrap: wrap;
        }
        .import-row {
            grid-template-columns: minmax(90px, 1fr) 60px 75px 70px;
            gap: 4px;
        }
    }
</style>
