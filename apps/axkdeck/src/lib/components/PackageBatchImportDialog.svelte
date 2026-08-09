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
        volumeNames: Record<string, string>;
        opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
        hasUnvalidatedChanges: boolean;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        completedFiles: number;
        totalFiles: number;
        progress: number;
        error: string;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
        onrename: (itemId: string, name: string) => void;
        ontoggleselected: (itemId: string, selected: boolean) => void;
        ontoggleall: (selected: boolean) => void;
        onopaquesequenceaction: (
            itemId: string,
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
        ontoggleselected,
        ontoggleall,
        onopaquesequenceaction,
        onreplan,
        oncancel,
        onconfirm,
    }: Props = $props();
    let selectAllCheckbox = $state<HTMLInputElement>();

    const busy = $derived(status === 'loading' || status === 'planning' || status === 'applying');
    const locked = $derived(status === 'applying');
    const selectedItems = $derived(items.filter((item) => item.selected));
    const selectedCount = $derived(selectedItems.length);
    const allSelected = $derived(items.length > 0 && selectedCount === items.length);
    const someSelected = $derived(selectedCount > 0 && !allSelected);
    const selectedPackageIndexById = $derived(
        new Map(selectedItems.map((item, packageIndex) => [item.id, packageIndex])),
    );
    const canImport = $derived(
        status === 'ready' && Boolean(plan?.valid) && !hasUnvalidatedChanges && selectedCount > 0,
    );
    const summaries = $derived(hasUnvalidatedChanges ? [] : (plan?.packages ?? []));
    const totalObjects = $derived(
        hasUnvalidatedChanges
            ? selectedItems.reduce((total, item) => total + item.inspection.objects.length, 0)
            : summaries.reduce((total, item) => total + item.objectCount, 0),
    );
    const totalPayload = $derived(
        hasUnvalidatedChanges
            ? selectedItems.reduce((total, item) => total + item.inspection.totalPayloadBytes, 0)
            : summaries.reduce((total, item) => total + item.payloadBytes, 0),
    );
    const capacityReports = $derived(
        hasUnvalidatedChanges ? [] : (plan?.sfsIndexCapacity.filter((capacity) => capacity.packages.length > 0) ?? []),
    );
    const capacityByPackage = $derived.by(() => {
        const result = new Map<number, (typeof capacityReports)[number]['packages'][number]>();
        for (const capacity of capacityReports) {
            for (const usage of capacity.packages) result.set(usage.packageIndex, usage);
        }
        return result;
    });
    const allocatableRecordsByPackage = $derived.by(() => {
        const result = new Map<number, number>();
        for (const capacity of capacityReports) {
            for (const usage of capacity.packages) result.set(usage.packageIndex, capacity.allocatableRecordSlots);
        }
        return result;
    });
    const visibleConflicts = $derived(
        hasUnvalidatedChanges
            ? []
            : (plan?.conflicts.filter(
                  (conflict) =>
                      conflict.code !== 'OPAQUE_SEQUENCE_DECISION_REQUIRED' &&
                      conflict.code !== 'SFS_RECORD_CAPACITY_EXHAUSTED',
              ) ?? []),
    );

    $effect(() => {
        if (selectAllCheckbox) selectAllCheckbox.indeterminate = someSelected;
    });

    function opaqueKey(itemId: string, nodeId: string): string {
        return `${itemId}:${nodeId}`;
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
                        <strong
                            >{selectedCount} of {items.length} package{items.length === 1 ? '' : 's'} selected</strong
                        >
                        <small>
                            {selectedCount} volume{selectedCount === 1 ? '' : 's'} will be created · {totalObjects}
                            objects · {formatStoredSize(totalPayload)}
                        </small>
                    </div>
                    <button class="secondary-button" type="button" disabled={busy} onclick={onchooseworkspace}>
                        Change files
                    </button>
                </section>

                {#if hasUnvalidatedChanges}
                    <p class="batch-plan-stale" role="status">
                        {selectedCount === 0
                            ? 'Select at least one package.'
                            : 'Selection or volume names changed. Check conflicts to recalculate.'}
                    </p>
                {/if}

                {#each capacityReports as capacity (capacity.partitionIndex)}
                    <section
                        class:capacity-unavailable={capacity.shortfallRecordSlots > 0}
                        class="capacity-summary"
                        aria-label={`SFS record capacity for partition ${capacity.partitionIndex + 1}`}
                    >
                        <div class="capacity-heading">
                            <strong>SFS record capacity</strong>
                            <small
                                >Partition {capacity.partitionIndex + 1} · {capacity.indexBlockCount} blocks ×
                                {capacity.recordsPerIndexBlock} records · {capacity.reservedRecordSlots} reserved</small
                            >
                        </div>
                        <div class="capacity-metrics">
                            <span>{capacity.freeRecordSlots} free</span>
                            <span>{capacity.requiredRecordSlots} required</span>
                            {#if capacity.shortfallRecordSlots > 0}
                                <strong>{capacity.shortfallRecordSlots} short</strong>
                            {:else}
                                <span>{capacity.remainingRecordSlots} remaining</span>
                            {/if}
                        </div>
                    </section>
                {/each}

                <div class="batch-table" role="table" aria-label="Volumes to create">
                    <div class="batch-table-header" role="row">
                        <span class="selection-cell" role="columnheader">
                            <input
                                bind:this={selectAllCheckbox}
                                type="checkbox"
                                checked={allSelected}
                                disabled={busy}
                                aria-label="Select all packages"
                                onchange={(event) => ontoggleall(event.currentTarget.checked)}
                            />
                        </span>
                        <span role="columnheader">Package</span>
                        <span role="columnheader">New volume</span>
                        <span role="columnheader">Contents</span>
                        <span role="columnheader">SFS records</span>
                    </div>
                    {#each items as item (item.id)}
                        {@const packageIndex = selectedPackageIndexById.get(item.id)}
                        {@const summary = summaries.find((entry) => entry.packageIndex === packageIndex)}
                        {@const recordUsage =
                            packageIndex === undefined ? undefined : capacityByPackage.get(packageIndex)}
                        {@const allocatableRecords =
                            packageIndex === undefined ? 0 : (allocatableRecordsByPackage.get(packageIndex) ?? 0)}
                        {@const cannotFitEmptyPartition =
                            recordUsage !== undefined && recordUsage.standaloneRequiredRecordSlots > allocatableRecords}
                        <div class:row-excluded={!item.selected} class="batch-table-row" role="row">
                            <div class="selection-cell" role="cell">
                                <input
                                    type="checkbox"
                                    checked={item.selected}
                                    disabled={busy}
                                    aria-label={`Include ${item.sourceName}`}
                                    onchange={(event) => ontoggleselected(item.id, event.currentTarget.checked)}
                                />
                            </div>
                            <div role="cell">
                                <strong>{item.sourceName}</strong>
                                <small>{formatStoredSize(item.inspection.totalPayloadBytes)}</small>
                            </div>
                            <div role="cell">
                                <label>
                                    <span class="sr-only">New volume name for {item.sourceName}</span>
                                    <input
                                        type="text"
                                        value={volumeNames[item.id] ??
                                            summary?.destinationVolumeName ??
                                            item.inspection.roots[0]?.displayName ??
                                            ''}
                                        maxlength="16"
                                        disabled={busy || !item.selected}
                                        oninput={(event) => onrename(item.id, event.currentTarget.value)}
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
                            <div
                                class:record-unavailable={Boolean(recordUsage?.shortfallRecordSlots)}
                                class:record-impossible={cannotFitEmptyPartition}
                                class="record-usage"
                                role="cell"
                                title={cannotFitEmptyPartition
                                    ? `Requires ${recordUsage?.standaloneRequiredRecordSlots} records on an empty partition; maximum ${allocatableRecords}`
                                    : undefined}
                            >
                                {#if !item.selected}
                                    <span>Not included</span>
                                {:else if hasUnvalidatedChanges}
                                    <span>Pending check</span>
                                {:else if recordUsage}
                                    <strong>{recordUsage.plannedRecordSlots}</strong>
                                    <small>
                                        {recordUsage.plannedObjectRecordSlots} objects
                                        {#if recordUsage.volumeScaffoldingRecordSlots > 0}
                                            + {recordUsage.volumeScaffoldingRecordSlots} volume
                                        {/if}
                                        {#if recordUsage.reusedObjectCount > 0}
                                            · {recordUsage.reusedObjectCount} reused
                                        {/if}
                                        {#if recordUsage.shortfallRecordSlots > 0}
                                            · {recordUsage.shortfallRecordSlots} short
                                        {/if}
                                    </small>
                                {:else}
                                    <span>—</span>
                                {/if}
                            </div>
                        </div>
                    {/each}
                </div>

                {#if !hasUnvalidatedChanges && plan?.opaqueSequences.length}
                    <section class="batch-sequence-decisions" aria-label="Sequence decisions">
                        {#each plan.opaqueSequences as sequence (`${sequence.packageIndex}:${sequence.nodeId}`)}
                            {@const packageIndex = sequence.packageIndex ?? 0}
                            {@const item = selectedItems[packageIndex]}
                            {@const selected = item
                                ? (opaqueSequenceActions[opaqueKey(item.id, sequence.nodeId)] ?? sequence.action)
                                : sequence.action}
                            {#if item}<fieldset>
                                    <legend>Sequence “{sequence.name || 'Unnamed'}” could not be decoded</legend>
                                    <small>Choose whether to preserve its original bytes or leave it out.</small>
                                    <label>
                                        <input
                                            type="radio"
                                            name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                            checked={selected === 'PRESERVE_UNCHANGED'}
                                            disabled={busy}
                                            onchange={() =>
                                                onopaquesequenceaction(item.id, sequence.nodeId, 'PRESERVE_UNCHANGED')}
                                        />
                                        Preserve unchanged
                                    </label>
                                    <label>
                                        <input
                                            type="radio"
                                            name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                            checked={selected === 'SKIP'}
                                            disabled={busy}
                                            onchange={() => onopaquesequenceaction(item.id, sequence.nodeId, 'SKIP')}
                                        />
                                        Skip Sequence
                                    </label>
                                </fieldset>{/if}
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
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={busy || selectedCount === 0}
                            onclick={onreplan}
                        >
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
                        : `Import ${selectedCount} package${selectedCount === 1 ? '' : 's'}`}
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

    .capacity-summary {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        margin-bottom: 14px;
        padding: 10px 12px;
        border: 1px solid var(--border-strong);
        border-radius: 6px;
        background: var(--color-panel-raised);
    }

    .capacity-heading {
        display: grid;
        gap: 3px;
    }

    .capacity-heading small,
    .record-usage small {
        color: var(--text-muted);
    }

    .capacity-metrics {
        display: flex;
        flex-wrap: wrap;
        justify-content: flex-end;
        gap: 6px 16px;
        white-space: nowrap;
    }

    .capacity-unavailable {
        border-color: #765d2d;
        background: rgb(150 108 26 / 10%);
    }

    .capacity-unavailable .capacity-metrics strong,
    .record-unavailable,
    .record-impossible {
        color: var(--color-danger);
    }

    .batch-summary div,
    .batch-table-row > div:first-child {
        display: grid;
        gap: 4px;
    }

    .batch-summary small,
    .batch-table-row small,
    .batch-sequence-decisions small {
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
        grid-template-columns:
            30px minmax(170px, 1.1fr) minmax(160px, 0.8fr) minmax(240px, 1.4fr)
            minmax(105px, 0.55fr);
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

    .selection-cell {
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .selection-cell input {
        margin: 0;
    }

    .row-excluded > :not(.selection-cell) {
        opacity: 0.55;
    }

    .object-counts {
        display: flex;
        flex-wrap: wrap;
        gap: 4px 12px;
        color: var(--text-muted);
        font-size: 12px;
    }

    .record-usage {
        display: grid;
        gap: 3px;
    }

    .batch-plan-stale {
        margin: 0 0 14px;
        color: var(--text-muted);
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
            grid-template-columns: 30px 1fr;
        }

        .batch-table-row > :not(.selection-cell) {
            grid-column: 2;
        }

        .capacity-summary {
            align-items: flex-start;
            flex-direction: column;
        }

        .capacity-metrics {
            justify-content: flex-start;
        }

        .selection-cell {
            grid-column: 1;
            grid-row: 1;
        }
    }
</style>
