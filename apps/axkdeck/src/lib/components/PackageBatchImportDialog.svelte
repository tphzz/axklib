<script lang="ts">
    import type { BatchPackageItem } from '../../features/import/packageBatchWorkflow.svelte';
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { ImageSessionPackageImportPlan, PackageOpaqueSequenceDecision } from '../transport';
    import Icon from './Icon.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';

    interface Props {
        partitionName: string;
        desktop: boolean;
        items: BatchPackageItem[];
        plan: ImageSessionPackageImportPlan | null;
        volumeNames: Record<number, string>;
        opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
        hasUnvalidatedChanges: boolean;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        completedFiles: number;
        totalFiles: number;
        progress: number;
        error: string;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
        onrename: (packageIndex: number, name: string) => void;
        onremove: (packageIndex: number) => void;
        onopaquesequenceaction: (
            packageIndex: number,
            nodeId: string,
            action: PackageOpaqueSequenceDecision['action'],
        ) => void;
        onreplan: () => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let {
        partitionName,
        desktop,
        items,
        plan,
        volumeNames,
        opaqueSequenceActions,
        hasUnvalidatedChanges,
        status,
        completedFiles,
        totalFiles,
        progress,
        error,
        onchooseworkspace,
        onchooselocal,
        onrename,
        onremove,
        onopaquesequenceaction,
        onreplan,
        oncancel,
        onconfirm,
    }: Props = $props();

    const busy = $derived(status === 'loading' || status === 'planning' || status === 'applying');
    const locked = $derived(status === 'applying');
    const canImport = $derived(
        status === 'ready' && Boolean(plan?.valid) && !hasUnvalidatedChanges && items.length > 0,
    );
    const summaries = $derived(plan?.packages ?? []);
    const totalObjects = $derived(summaries.reduce((total, item) => total + item.objectCount, 0));
    const totalPayload = $derived(summaries.reduce((total, item) => total + item.payloadBytes, 0));
    const visibleConflicts = $derived(
        plan?.conflicts.filter((conflict) => conflict.code !== 'OPAQUE_SEQUENCE_DECISION_REQUIRED') ?? [],
    );

    function opaqueKey(packageIndex: number, nodeId: string): string {
        return `${packageIndex}:${nodeId}`;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide batch-package-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="batch-package-title"
        use:modal={{ onescape: locked ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2 id="batch-package-title">Import volume packages</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={locked} onclick={oncancel}>×</button>
        </header>

        <div class="batch-package-content">
            {#if items.length === 0 && status === 'choosing'}
                <ImportSourceChoice
                    label="Volume package sources"
                    heading="Choose volume packages"
                    description={`Create volumes in ${partitionName} from selected .axkvol packages.`}
                    workspaceDetail="Select one or more .axkvol files"
                    computerDetail="Select local .axkvol files and upload them"
                    computerAvailable={desktop}
                    {onchooseworkspace}
                    {onchooselocal}
                />
            {:else if status === 'loading'}
                <section class="batch-progress" aria-live="polite">
                    <strong>Preparing volume packages</strong>
                    <progress value={Math.max(completedFiles, progress)} max={Math.max(1, totalFiles)}></progress>
                    <small>{completedFiles} of {totalFiles} files inspected</small>
                </section>
            {:else}
                <section class="batch-summary" aria-label="Batch summary">
                    <div>
                        <strong>{items.length} volume{items.length === 1 ? '' : 's'} will be created</strong>
                        <small>{totalObjects} objects · {formatStoredSize(totalPayload)}</small>
                    </div>
                    <button class="secondary-button" type="button" disabled={busy} onclick={onchooseworkspace}>
                        Change files
                    </button>
                </section>

                <div class="batch-table" role="table" aria-label="Volumes to create">
                    <div class="batch-table-header" role="row">
                        <span role="columnheader">Package</span>
                        <span role="columnheader">New volume</span>
                        <span role="columnheader">Contents</span>
                        <span aria-hidden="true"></span>
                    </div>
                    {#each items as item, packageIndex (`${packageIndex}:${item.inspection.packageId}`)}
                        {@const summary = summaries.find((entry) => entry.packageIndex === packageIndex)}
                        <div class="batch-table-row" role="row">
                            <div role="cell">
                                <strong>{item.sourceName}</strong>
                                <small>{formatStoredSize(item.inspection.totalPayloadBytes)}</small>
                            </div>
                            <div role="cell">
                                <label>
                                    <span class="sr-only">New volume name for {item.sourceName}</span>
                                    <input
                                        type="text"
                                        value={volumeNames[packageIndex] ?? summary?.destinationVolumeName ?? ''}
                                        maxlength="16"
                                        disabled={busy}
                                        oninput={(event) => onrename(packageIndex, event.currentTarget.value)}
                                    />
                                </label>
                            </div>
                            <div class="object-counts" role="cell">
                                {#if summary}
                                    <span>{summary.objectCounts.programs} Programs</span>
                                    <span>{summary.objectCounts.sampleBanks} Sample Banks</span>
                                    <span>{summary.objectCounts.samples} Samples</span>
                                    <span>{summary.objectCounts.waveData} Wave Data</span>
                                    <span>{summary.objectCounts.sequences} Sequences</span>
                                {:else}
                                    <span>{item.inspection.objects.length} objects</span>
                                {/if}
                            </div>
                            <button
                                class="icon-button remove-package"
                                type="button"
                                aria-label={`Remove ${item.sourceName}`}
                                title="Remove package"
                                disabled={busy}
                                onclick={() => onremove(packageIndex)}
                            >
                                <Icon name="trash" size={14} />
                            </button>
                        </div>
                    {/each}
                </div>

                {#if plan?.opaqueSequences.length}
                    <section class="batch-sequence-decisions" aria-label="Sequence decisions">
                        {#each plan.opaqueSequences as sequence (`${sequence.packageIndex}:${sequence.nodeId}`)}
                            {@const packageIndex = sequence.packageIndex ?? 0}
                            {@const selected =
                                opaqueSequenceActions[opaqueKey(packageIndex, sequence.nodeId)] ?? sequence.action}
                            <fieldset>
                                <legend>Sequence “{sequence.name || 'Unnamed'}” could not be decoded</legend>
                                <small>Choose whether to preserve its original bytes or leave it out.</small>
                                <label>
                                    <input
                                        type="radio"
                                        name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                        checked={selected === 'PRESERVE_UNCHANGED'}
                                        disabled={busy}
                                        onchange={() =>
                                            onopaquesequenceaction(packageIndex, sequence.nodeId, 'PRESERVE_UNCHANGED')}
                                    />
                                    Preserve unchanged
                                </label>
                                <label>
                                    <input
                                        type="radio"
                                        name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                        checked={selected === 'SKIP'}
                                        disabled={busy}
                                        onchange={() => onopaquesequenceaction(packageIndex, sequence.nodeId, 'SKIP')}
                                    />
                                    Skip Sequence
                                </label>
                            </fieldset>
                        {/each}
                    </section>
                {/if}

                {#if visibleConflicts.length > 0}
                    <section class="batch-conflicts" role="alert">
                        <strong
                            >{visibleConflicts.length} issue{visibleConflicts.length === 1 ? '' : 's'} prevent import</strong
                        >
                        {#each visibleConflicts as conflict (`${conflict.packageIndex}:${conflict.code}:${conflict.nodeId}`)}
                            <p>{conflict.message}</p>
                        {/each}
                    </section>
                {:else if plan?.valid && !hasUnvalidatedChanges && !error}
                    <p class="batch-ready"><Icon name="check" size={14} /> Ready to import</p>
                {/if}

                {#if hasUnvalidatedChanges || visibleConflicts.length > 0}
                    <div class="batch-check-actions">
                        {#if hasUnvalidatedChanges}<small>Volume name changes must be checked before import.</small
                            >{/if}
                        <button class="secondary-button" type="button" disabled={busy} onclick={onreplan}>
                            Check conflicts
                        </button>
                    </div>
                {/if}
            {/if}
            {#if status === 'planning'}<p class="dialog-progress" role="status">Planning batch import…</p>{/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={locked} onclick={oncancel}>Cancel</button>
            {#if items.length > 0}
                <button class="primary-button" type="button" disabled={!canImport} onclick={onconfirm}>
                    {status === 'applying'
                        ? 'Importing…'
                        : `Import ${items.length} package${items.length === 1 ? '' : 's'}`}
                </button>
            {/if}
        </footer>
    </div>
</div>

<style>
    .batch-package-dialog {
        width: min(1080px, calc(100vw - 32px));
        max-height: min(860px, calc(100vh - 32px));
    }

    .batch-package-content {
        min-height: 0;
        overflow: auto;
        padding: 18px;
    }

    .batch-progress,
    .batch-summary,
    .batch-check-actions {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .batch-progress {
        min-height: 180px;
        flex-direction: column;
        justify-content: center;
    }

    .batch-progress progress {
        width: min(420px, 100%);
    }

    .batch-summary {
        justify-content: space-between;
        margin-bottom: 14px;
    }

    .batch-summary div,
    .batch-table-row > div:first-child {
        display: grid;
        gap: 4px;
    }

    .batch-summary small,
    .batch-table-row small,
    .batch-sequence-decisions small,
    .batch-check-actions small {
        color: var(--text-muted);
    }

    .batch-table {
        border: 1px solid var(--border-strong);
        border-radius: 6px;
        overflow: hidden;
    }

    .batch-table-header,
    .batch-table-row {
        display: grid;
        grid-template-columns: minmax(180px, 1.2fr) minmax(170px, 0.8fr) minmax(260px, 1.5fr) 34px;
        align-items: center;
        gap: 14px;
        padding: 10px 12px;
    }

    .batch-table-header {
        color: var(--text-muted);
        background: var(--color-panel-raised);
        font-size: 12px;
    }

    .batch-table-row + .batch-table-row {
        border-top: 1px solid var(--border-subtle);
    }

    .batch-table-row input[type='text'] {
        width: 100%;
        min-width: 0;
    }

    .object-counts {
        display: flex;
        flex-wrap: wrap;
        gap: 4px 12px;
        color: var(--text-muted);
        font-size: 12px;
    }

    .remove-package {
        color: var(--color-danger);
    }

    .batch-sequence-decisions,
    .batch-conflicts {
        display: grid;
        gap: 10px;
        margin-top: 14px;
        padding: 12px;
        border: 1px solid #765d2d;
        border-radius: 6px;
        background: rgb(150 108 26 / 10%);
    }

    .batch-sequence-decisions fieldset {
        display: grid;
        gap: 8px;
        border: 0;
        padding: 0;
    }

    .batch-sequence-decisions label {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .batch-conflicts p {
        margin: 0;
    }

    .batch-ready {
        display: flex;
        align-items: center;
        gap: 6px;
        color: #85d8ad;
    }

    .batch-check-actions {
        justify-content: flex-end;
        margin-top: 12px;
    }

    @media (max-width: 760px) {
        .batch-table-header {
            display: none;
        }

        .batch-table-row {
            grid-template-columns: 1fr 34px;
        }

        .batch-table-row > :not(.remove-package) {
            grid-column: 1;
        }

        .remove-package {
            grid-column: 2;
            grid-row: 1;
        }
    }
</style>
