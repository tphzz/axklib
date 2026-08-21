<script lang="ts">
    import type { BatchPackageItem, PackageBatchDestinationStrategy } from '../../features/import/packageBatchTypes';
    import { formatStoredSize } from '../formatBytes';
    import type { ImageSessionPackageImportPlan } from '../transport';

    interface Props {
        items: BatchPackageItem[];
        plan: ImageSessionPackageImportPlan | null;
        destinationStrategy: PackageBatchDestinationStrategy;
        destinationVolumeName: string;
        volumeNames: Record<string, string>;
        hasUnvalidatedChanges: boolean;
        busy: boolean;
        onrenamevolume: (itemId: string, name: string) => void;
        ontoggleselected: (itemId: string, selected: boolean) => void;
        ontoggleall: (selected: boolean) => void;
    }

    let {
        items,
        plan,
        destinationStrategy,
        destinationVolumeName,
        volumeNames,
        hasUnvalidatedChanges,
        busy,
        onrenamevolume,
        ontoggleselected,
        ontoggleall,
    }: Props = $props();
    let selectAllCheckbox = $state<HTMLInputElement>();

    const selectedItems = $derived(items.filter((item) => item.selected));
    const allSelected = $derived(items.length > 0 && selectedItems.length === items.length);
    const someSelected = $derived(selectedItems.length > 0 && !allSelected);
    const packageIndexById = $derived(new Map(selectedItems.map((item, index) => [item.id, index])));
    const summaries = $derived(hasUnvalidatedChanges ? [] : (plan?.packages ?? []));
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

    $effect(() => {
        if (selectAllCheckbox) selectAllCheckbox.indeterminate = someSelected;
    });
</script>

{#each capacityReports as capacity (capacity.partitionIndex)}
    <section
        class:capacity-unavailable={capacity.shortfallRecordSlots > 0}
        class="capacity-summary"
        aria-label={`SFS record capacity for partition ${capacity.partitionIndex + 1}`}
    >
        <div class="capacity-heading">
            <strong>SFS record capacity</strong>
            <small>
                Partition {capacity.partitionIndex + 1} · {capacity.indexBlockCount} blocks ×
                {capacity.recordsPerIndexBlock} records · {capacity.reservedRecordSlots} reserved
            </small>
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

<div class="batch-table" role="table" aria-label="Packages to import">
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
        <span role="columnheader">Destination</span>
        <span role="columnheader">Contents</span>
        <span role="columnheader">SFS records</span>
    </div>
    {#each items as item (item.id)}
        {@const packageIndex = packageIndexById.get(item.id)}
        {@const summary = summaries.find((entry) => entry.packageIndex === packageIndex)}
        {@const recordUsage = packageIndex === undefined ? undefined : capacityByPackage.get(packageIndex)}
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
                {#if destinationStrategy === 'separate'}
                    <label>
                        <span class="sr-only">New volume name for {item.sourceName}</span>
                        <input
                            class="dialog-field-control"
                            type="text"
                            value={volumeNames[item.id] ??
                                summary?.destinationVolumeName ??
                                item.inspection.roots[0]?.displayName ??
                                ''}
                            maxlength="16"
                            disabled={busy || !item.selected}
                            oninput={(event) => onrenamevolume(item.id, event.currentTarget.value)}
                        />
                    </label>
                {:else if item.selected}
                    <span>{destinationVolumeName || 'Choose a volume'}</span>
                {:else}
                    <span>Not included</span>
                {/if}
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
                    <span>{recordUsage.plannedRecordSlots}</span>
                    <small>
                        {recordUsage.plannedObjectRecordSlots} objects
                        {#if recordUsage.volumeScaffoldingRecordSlots > 0}
                            + {recordUsage.volumeScaffoldingRecordSlots} volume
                        {/if}
                        {#if recordUsage.reusedObjectCount > 0}· {recordUsage.reusedObjectCount} reused{/if}
                        {#if recordUsage.shortfallRecordSlots > 0}· {recordUsage.shortfallRecordSlots} short{/if}
                    </small>
                {:else}
                    <span>—</span>
                {/if}
            </div>
        </div>
    {/each}
</div>

<style>
    .capacity-summary {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        margin-bottom: 10px;
        padding: 8px 10px;
        border: 1px solid var(--border-strong);
        border-radius: 6px;
        background: var(--color-panel-raised);
    }

    .capacity-heading,
    .record-usage,
    .batch-table-row > div:nth-child(2) {
        display: grid;
        gap: 3px;
    }

    small {
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

    .capacity-unavailable strong,
    .record-unavailable,
    .record-impossible {
        color: var(--color-danger);
    }

    .batch-table {
        overflow: hidden;
        border: 1px solid var(--border-strong);
        border-radius: 6px;
    }

    .batch-table-header,
    .batch-table-row {
        display: grid;
        grid-template-columns: 30px minmax(170px, 1.1fr) minmax(160px, 0.8fr) minmax(240px, 1.4fr) minmax(
                105px,
                0.55fr
            );
        gap: 12px;
        padding-inline: 10px;
    }

    .batch-table-header {
        align-items: center;
        padding-block: 7px;
        color: var(--text-muted);
        background: var(--color-panel-raised);
        font-size: 12px;
    }

    .batch-table-row {
        align-items: start;
        padding-block: 6px;
    }

    .batch-table-row + .batch-table-row {
        border-top: 1px solid var(--border-subtle);
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
        gap: 2px 10px;
        color: var(--text-muted);
        font-size: 12px;
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
    }
</style>
