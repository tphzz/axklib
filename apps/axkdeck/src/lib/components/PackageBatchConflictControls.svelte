<script lang="ts">
    import { batchDecisionKey } from '../../features/import/packageBatchPlanning';
    import type { BatchPackageItem } from '../../features/import/packageBatchTypes';
    import type { ImageSessionPackageImportPlan } from '../transport';

    interface Props {
        items: BatchPackageItem[];
        plan: ImageSessionPackageImportPlan;
        renames: Record<string, string>;
        programSlots: Record<string, number>;
        disabled: boolean;
        onrename: (itemId: string, nodeId: string, name: string) => void;
        onprogramslot: (itemId: string, nodeId: string, slot: number) => void;
        onprogramstart: (placementId: string, start: number) => void;
    }

    const renameConflictCodes = new Set([
        'SFS_NAME_CONFLICT',
        'SFS_TARGET_NAME_AMBIGUOUS',
        'FAT12_NAME_CONFLICT',
        'FAT12_TARGET_NAME_AMBIGUOUS',
        'ISO9660_NAME_CONFLICT',
        'ISO9660_TARGET_NAME_AMBIGUOUS',
    ]);

    let { items, plan, renames, programSlots, disabled, onrename, onprogramslot, onprogramstart }: Props = $props();

    const placementKeys = $derived(
        new Set(
            plan.programSlotPlacements.flatMap((placement) =>
                placement.mappings.map((mapping) => `${mapping.packageIndex}:${mapping.nodeId}`),
            ),
        ),
    );
    const renameActions = $derived(
        Array.from(
            new Map(
                plan.actions
                    .filter(
                        (action) =>
                            !placementKeys.has(`${action.packageIndex}:${action.nodeId}`) &&
                            action.actions.includes('CONFLICT') &&
                            plan.conflicts.some(
                                (conflict) =>
                                    conflict.packageIndex === action.packageIndex &&
                                    conflict.nodeId === action.nodeId &&
                                    renameConflictCodes.has(conflict.code),
                            ),
                    )
                    .map((action) => [`${action.packageIndex}:${action.nodeId}`, action]),
            ).values(),
        ),
    );

    function formatSlot(slot: number): string {
        return String(slot).padStart(3, '0');
    }

    function formatRanges(ranges: { first: number; last: number }[]): string {
        if (ranges.length === 0) return 'None';
        return ranges
            .map((range) =>
                range.first === range.last
                    ? formatSlot(range.first)
                    : `${formatSlot(range.first)}–${formatSlot(range.last)}`,
            )
            .join(', ');
    }

    function assignmentKey(packageIndex: number, nodeId: string): string {
        const item = items[packageIndex];
        return item ? batchDecisionKey(item.id, nodeId) : '';
    }

    function placementHasChanges(placement: ImageSessionPackageImportPlan['programSlotPlacements'][number]): boolean {
        return placement.mappings.some((mapping) => {
            const key = assignmentKey(mapping.packageIndex, mapping.nodeId);
            return key.length > 0 && (programSlots[key] ?? mapping.destinationSlot) !== mapping.destinationSlot;
        });
    }
</script>

{#if plan.programSlotPlacements.length > 0}
    <section class="batch-program-slots" aria-label="Program slot placement">
        {#each plan.programSlotPlacements as placement (placement.placementId)}
            {@const firstMapping = placement.mappings[0]}
            {@const firstKey = firstMapping ? assignmentKey(firstMapping.packageIndex, firstMapping.nodeId) : ''}
            <div class:pending={!placement.applied || placementHasChanges(placement)} class="program-placement">
                <div class="placement-heading">
                    <strong>Program slots</strong>
                    <small>
                        {placement.mode === 'UNAVAILABLE'
                            ? 'Not enough free slots'
                            : placementHasChanges(placement)
                              ? 'Needs check'
                              : placement.applied
                                ? 'Checked'
                                : 'Suggested'}
                    </small>
                </div>
                <dl>
                    <div>
                        <dt>Occupied</dt>
                        <dd>{formatRanges(placement.occupiedRanges)}</dd>
                    </div>
                    <div>
                        <dt>Package</dt>
                        <dd>{formatRanges(placement.sourceRanges)}</dd>
                    </div>
                    <div>
                        <dt>Destination</dt>
                        <dd>{formatRanges(placement.destinationRanges)}</dd>
                    </div>
                </dl>
                {#if placement.mode === 'UNAVAILABLE'}
                    <p>
                        {placement.requiredSlotCount} slots are required, but only {placement.availableSlotCount} are available.
                    </p>
                {:else if placement.mode === 'CONTIGUOUS'}
                    <label class="slot-control">
                        <span>Destination start</span>
                        <input
                            class="dialog-field-control"
                            type="number"
                            min="1"
                            max={128 - placement.requiredSlotCount + 1}
                            {disabled}
                            value={programSlots[firstKey] ?? placement.suggestedStartSlot ?? 1}
                            oninput={(event) =>
                                onprogramstart(placement.placementId, event.currentTarget.valueAsNumber)}
                        />
                    </label>
                {:else}
                    <div class="slot-exceptions">
                        {#each placement.mappings as mapping (`${mapping.packageIndex}:${mapping.nodeId}`)}
                            {@const item = items[mapping.packageIndex]}
                            {@const key = assignmentKey(mapping.packageIndex, mapping.nodeId)}
                            {#if item}
                                <label class="slot-control">
                                    <span>{item.sourceName} · Program {formatSlot(mapping.sourceSlot)}</span>
                                    <input
                                        class="dialog-field-control"
                                        type="number"
                                        min="1"
                                        max="128"
                                        {disabled}
                                        value={programSlots[key] ?? mapping.destinationSlot}
                                        oninput={(event) =>
                                            onprogramslot(item.id, mapping.nodeId, event.currentTarget.valueAsNumber)}
                                    />
                                </label>
                            {/if}
                        {/each}
                    </div>
                {/if}
            </div>
        {/each}
    </section>
{/if}

{#if renameActions.length > 0}
    <section class="batch-renames" aria-label="Destination names">
        <strong>Choose unused destination names</strong>
        {#each renameActions as action (`${action.packageIndex}:${action.actionId}`)}
            {@const item = items[action.packageIndex]}
            {@const key = item ? batchDecisionKey(item.id, action.nodeId) : ''}
            {#if item}
                <label>
                    <span>{item.sourceName} · {action.sourceName}</span>
                    <input
                        class="dialog-field-control"
                        value={renames[key] ?? action.destinationName}
                        maxlength="16"
                        {disabled}
                        oninput={(event) => onrename(item.id, action.nodeId, event.currentTarget.value)}
                    />
                </label>
            {/if}
        {/each}
    </section>
{/if}

<style>
    .batch-program-slots,
    .batch-renames {
        display: grid;
        gap: 10px;
        margin-top: 10px;
    }

    .program-placement,
    .batch-renames {
        padding: 10px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: rgb(255 255 255 / 2%);
    }

    .program-placement.pending {
        border-color: #765d2d;
    }

    .placement-heading {
        display: flex;
        justify-content: space-between;
        gap: 12px;
    }

    .placement-heading small,
    dt {
        color: var(--color-text-muted);
    }

    dl {
        display: grid;
        gap: 4px;
        margin: 8px 0;
    }

    dl div,
    .slot-control,
    .batch-renames label {
        display: grid;
        grid-template-columns: minmax(150px, 1fr) minmax(90px, 0.4fr);
        align-items: center;
        gap: 10px;
    }

    dt,
    dd,
    p {
        margin: 0;
    }

    .slot-exceptions {
        display: grid;
        gap: 6px;
    }

    @media (max-width: 620px) {
        dl div,
        .slot-control,
        .batch-renames label {
            grid-template-columns: 1fr;
        }
    }
</style>
