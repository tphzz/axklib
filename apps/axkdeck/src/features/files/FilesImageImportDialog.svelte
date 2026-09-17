<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import { formatStoredSize } from '../../lib/formatBytes';
    import FilesystemNameField from './FilesystemNameField.svelte';
    import FilesImportFooter from './FilesImportFooter.svelte';
    import type { FilesImageImportWorkflow } from './imageImportWorkflow.svelte';
    import { importPaths } from './importReview';
    let { workflow }: { workflow: FilesImageImportWorkflow } = $props();
    const importer = $derived(workflow.importer);
    let collapsed = $state<string[]>([]);
    let page = $state(0);
    const rows = $derived.by(() =>
        workflow.visibleGroups.flatMap((group, groupIndex) => {
            const paths = importPaths(group.rows);
            return group.rows.flatMap((row, index) => {
                let parent = row.parent;
                while (parent !== null) {
                    if (collapsed.includes(`${workflow.mode}:${groupIndex}:${parent}`)) return [];
                    parent = group.rows[parent].parent;
                }
                return [{ row, groupIndex, index, path: paths[index], group }];
            });
        }),
    );
    const pageCount = $derived(Math.max(1, Math.ceil(rows.length / 100)));
    $effect(() => {
        if (page >= pageCount) page = pageCount - 1;
    });
    function collapse(key: string): void {
        collapsed = collapsed.includes(key) ? collapsed.filter((value) => value !== key) : [...collapsed, key];
    }
    const outcome = {
        CREATE_FILE: 'Create',
        SKIP_FILE: 'Skip',
        REPLACE_FILE: 'Replace',
        CONFLICT: 'Conflict',
        CREATE_DIRECTORY: 'Create',
        MERGE_DIRECTORY: 'Merge',
    };
</script>

{#if importer.target}
    <div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
        <div
            class="dialog-shell image-import-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Import floppy files"
            aria-busy={workflow.loading || importer.busy}
            use:modal={{ onescape: () => workflow.close() }}
        >
            <header class="dialog-header">
                <h2>Import floppy files</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={!importer.canDismiss}
                    onclick={() => workflow.close()}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="image-import-content">
                <div class="image-import-summary">
                    <strong title={importer.target.path || '/'}>{importer.target.path || '/'}</strong><span
                        >{importer.rows.length} entries · {formatStoredSize(
                            importer.rows.reduce((size, row) => size + (row.snapshot?.sizeBytes ?? 0), 0),
                        )}</span
                    >
                </div>
                <div class="image-import-toolbar">
                    <div class="dialog-segmented-control" role="group" aria-label="Import mode">
                        {#each ['File', 'Contents'] as mode}<button
                                type="button"
                                aria-pressed={workflow.mode === mode}
                                disabled={!workflow.editable}
                                onclick={() => {
                                    page = 0;
                                    workflow.setMode(mode as 'File' | 'Contents');
                                }}>{mode}</button
                            >{/each}
                    </div>
                </div>
                <div class="image-table" role="table" aria-label="Floppy import entries">
                    <div class="image-row heading" role="row">
                        <input
                            class="dialog-checkbox"
                            type="checkbox"
                            aria-label="Select all entries"
                            checked={workflow.allSelected}
                            indeterminate={!workflow.allSelected && workflow.selectedCount > 0}
                            disabled={!workflow.editable || !workflow.totalCount}
                            onchange={(event) => workflow.selectAll(event.currentTarget.checked)}
                        />
                        <span role="columnheader">Name</span><span role="columnheader" class="size">Size</span><span
                            role="columnheader">If exists</span
                        ><span role="columnheader">Result</span>
                    </div>
                    <div class="image-rows" role="rowgroup">
                        {#each rows.slice(page * 100, (page + 1) * 100) as item, offset (`${workflow.mode}:${item.groupIndex}:${item.index}`)}
                            {@const key = `${item.groupIndex}:${item.index}`}
                            {@const reviewIndex = workflow.mapping.get(key)}
                            {@const decision = reviewIndex === undefined ? null : importer.rows[reviewIndex]?.decision}
                            {@const selection = workflow.selection(item.groupIndex, item.index)}
                            {#if offset === 0 || rows[page * 100 + offset - 1].groupIndex !== item.groupIndex}<h3
                                    title={item.group.name}
                                >
                                    {item.group.name}
                                </h3>{/if}
                            <div class="image-row" role="row">
                                <input
                                    class="dialog-checkbox"
                                    type="checkbox"
                                    aria-label={`Import ${item.group.name}: ${item.path.join('/')}`}
                                    checked={selection === 'all'}
                                    indeterminate={selection === 'some'}
                                    disabled={!workflow.editable}
                                    onchange={(event) =>
                                        workflow.toggle(item.groupIndex, item.index, event.currentTarget.checked)}
                                />
                                <div
                                    class="image-name"
                                    role="cell"
                                    style:padding-left={`${Math.min(item.path.length - 1, 6) * 12}px`}
                                >
                                    {#if item.row.directory}<button
                                            class="icon-button"
                                            type="button"
                                            aria-label={`${collapsed.includes(`${workflow.mode}:${key}`) ? 'Expand' : 'Collapse'} ${item.path.join('/')}`}
                                            title={collapsed.includes(`${workflow.mode}:${key}`)
                                                ? 'Expand folder'
                                                : 'Collapse folder'}
                                            aria-expanded={!collapsed.includes(`${workflow.mode}:${key}`)}
                                            onclick={() => collapse(`${workflow.mode}:${key}`)}
                                            ><Icon name="chevron" size={12} /></button
                                        >{:else}<Icon name="file" size={12} />{/if}
                                    <FilesystemNameField
                                        value={item.row.name}
                                        label={`Filename ${item.groupIndex + 1}.${item.index + 1}`}
                                        capabilities={importer.capabilities!}
                                        immediate
                                        disabled={!workflow.editable}
                                        title={item.path.join('/')}
                                        error={workflow.errors.get(key) ??
                                            (decision?.action === 'CONFLICT' ? decision.issue : undefined)}
                                        onchange={(name) => workflow.rename(item.groupIndex, item.index, name)}
                                    />
                                </div>
                                <span class="size" role="cell"
                                    >{item.row.snapshot ? formatStoredSize(item.row.snapshot.sizeBytes) : '-'}</span
                                >
                                <span role="cell"
                                    >{#if item.row.directory}Merge{:else}<select
                                            class="dialog-field-control"
                                            aria-label={`If file exists ${item.groupIndex + 1}.${item.index + 1}`}
                                            disabled={!workflow.editable}
                                            value={item.row.conflict}
                                            onchange={(event) =>
                                                workflow.conflict(
                                                    item.groupIndex,
                                                    item.index,
                                                    event.currentTarget.value === 'REPLACE' ? 'REPLACE' : 'SKIP',
                                                )}
                                            ><option value="SKIP">Skip</option><option value="REPLACE">Replace</option
                                            ></select
                                        >{/if}</span
                                >
                                <span
                                    role="cell"
                                    class="result"
                                    class:dialog-error={decision?.action === 'CONFLICT'}
                                    title={decision?.issue ?? ''}
                                    >{reviewIndex === undefined
                                        ? 'Excluded'
                                        : decision
                                          ? outcome[decision.action]
                                          : 'Not reviewed'}</span
                                >
                            </div>
                        {/each}
                    </div>
                </div>
                {#if pageCount > 1}<nav aria-label="Floppy entry pages">
                        <button
                            class="icon-button previous"
                            type="button"
                            title="Previous page"
                            aria-label="Previous page"
                            disabled={page === 0}
                            onclick={() => (page -= 1)}><Icon name="chevron" size={14} /></button
                        ><span>{page + 1} / {pageCount}</span><button
                            class="icon-button"
                            type="button"
                            title="Next page"
                            aria-label="Next page"
                            disabled={page + 1 >= pageCount}
                            onclick={() => (page += 1)}><Icon name="chevron" size={14} /></button
                        >
                    </nav>{/if}
                {#if workflow.error || workflow.visibleGroups.some((group) => group.error) || importer.warnings.length || importer.rows.some((row) => row.decision?.issue)}
                    <div class="dialog-results image-errors" role="alert">
                        {#if workflow.error}<p>{workflow.error}</p>{/if}
                        {#each workflow.visibleGroups.filter((group) => group.error) as group}<p>
                                {group.name}: {group.error}
                            </p>{/each}
                        {#each importer.rows.filter((row) => row.decision?.issue) as row}<p>
                                {row.name}: {row.decision?.issue}
                            </p>{/each}
                        {#each importer.warnings as warning}<p>{warning}</p>{/each}
                    </div>
                {/if}
            </div>
            <FilesImportFooter
                workflow={importer}
                dismiss={() => workflow.close()}
                blocked={!workflow.canReview}
                review={() => workflow.review()}
            />
        </div>
    </div>
{/if}

<style>
    .image-import-dialog {
        width: min(900px, calc(100vw - 32px));
        height: min(640px, calc(100dvh - 32px));
        display: flex;
        flex-direction: column;
    }
    .image-import-content {
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 12px;
        flex: 1;
        min-height: 0;
    }
    .image-import-summary,
    .image-import-toolbar {
        display: flex;
        align-items: center;
        gap: 12px;
        font-size: var(--dialog-body-font-size);
    }
    .image-import-summary strong {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .image-table {
        flex: 1;
        min-height: 0;
        display: flex;
        flex-direction: column;
    }
    .image-row {
        display: grid;
        grid-template-columns: 16px minmax(140px, 1fr) 64px 82px 80px;
        gap: 8px;
        align-items: start;
        padding-block: 4px;
        border-bottom: 1px solid var(--color-border);
        font-size: var(--dialog-body-font-size);
    }
    .image-row > * {
        min-width: 0;
    }
    .image-name {
        display: flex;
        align-items: center;
        gap: 4px;
    }
    .image-name :global(.filesystem-name-field) {
        flex: 1;
        min-width: 0;
    }
    .image-name [aria-expanded='true'] :global(svg) {
        transform: rotate(90deg);
    }
    .image-row select {
        width: 100%;
    }
    .image-rows,
    .heading {
        padding-right: calc(8px + var(--overlay-scrollbar-clearance));
        scrollbar-gutter: stable;
    }
    .heading {
        overflow-y: hidden;
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .image-rows {
        flex: 1;
        min-height: 0;
        overflow: auto;
    }
    h3 {
        font-size: var(--dialog-body-font-size);
        margin: 8px 0 4px;
        overflow-wrap: anywhere;
    }
    .size {
        text-align: right;
    }
    .result {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .image-errors {
        max-height: 90px;
        overflow: auto;
        font-size: var(--dialog-body-font-size);
    }
    .image-errors p {
        margin: 0 0 4px;
        overflow-wrap: anywhere;
    }
    nav {
        display: flex;
        align-items: center;
        justify-content: flex-end;
        gap: 8px;
        font-size: var(--dialog-metadata-font-size);
    }
    .previous {
        transform: rotate(180deg);
    }
    @media (max-width: 600px) {
        .image-row {
            grid-template-columns: 16px minmax(90px, 1fr) 50px 65px;
            gap: 4px;
        }
        .result,
        .heading span:last-child {
            display: none;
        }
    }
</style>
