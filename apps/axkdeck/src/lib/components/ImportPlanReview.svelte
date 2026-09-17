<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import type { ImageSessionPackageImportPlan, PackageOpaqueSequenceDecision } from '../transport';
    import type { ImportPartitionOption } from '../../features/import/packageDestinations';
    import { importCapacityGroups, isImportSpaceConflict } from '../importCapacity';
    import ImportCapacityIssues from './ImportCapacityIssues.svelte';
    let {
        plan,
        partitions = [],
        busy,
        targetName,
        renames,
        programSlots,
        opaqueSequenceActions,
        onrename,
        onprogramslot,
        onprogramstart,
        onopaquesequenceaction,
    }: {
        plan: ImageSessionPackageImportPlan | null;
        partitions?: ImportPartitionOption[];
        busy: boolean;
        targetName: string;
        renames: Record<string, string>;
        programSlots: Record<string, number>;
        opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
        onrename: (id: string, name: string) => void;
        onprogramslot: (id: string, slot: number) => void;
        onprogramstart: (id: string, slot: number) => void;
        onopaquesequenceaction: (id: string, action: PackageOpaqueSequenceDecision['action']) => void;
    } = $props();
    const capacityGroups = $derived(importCapacityGroups(plan?.conflicts ?? [], partitions));
    const renameConflictCodes = new Set([
        'SFS_NAME_CONFLICT',
        'SFS_TARGET_NAME_AMBIGUOUS',
        'FAT12_NAME_CONFLICT',
        'FAT12_TARGET_NAME_AMBIGUOUS',
        'ISO9660_NAME_CONFLICT',
        'ISO9660_TARGET_NAME_AMBIGUOUS',
    ]);

    const renameConflictNodes = $derived(
        new Set(
            plan?.conflicts
                .filter((conflict) => renameConflictCodes.has(conflict.code) && conflict.nodeId)
                .map((conflict) => conflict.nodeId) ?? [],
        ),
    );
    const placementNodeIds = $derived(
        new Set(
            plan?.programSlotPlacements.flatMap((placement) => placement.mappings.map((mapping) => mapping.nodeId)),
        ),
    );
    const placementIssues = $derived(
        (plan?.programSlotPlacements ?? []).filter(
            (placement) =>
                !placement.applied ||
                placement.mode === 'UNAVAILABLE' ||
                placement.mappings.some((mapping) => mapping.requiresUserAction),
        ),
    );
    const editableProgramPlacements = $derived(
        (plan?.programSlotPlacements ?? []).filter((placement) => placement.mode !== 'UNAVAILABLE'),
    );
    const renameActions = $derived(
        Array.from(
            new Map(
                (plan?.actions ?? [])
                    .filter(
                        (action) =>
                            !placementNodeIds.has(action.nodeId) &&
                            renameConflictNodes.has(action.nodeId) &&
                            action.actions.includes('CONFLICT'),
                    )
                    .map((action) => [action.nodeId, action]),
            ).values(),
        ),
    );
    const nonRenameConflicts = $derived(
        Array.from(
            new Map(
                (plan?.conflicts ?? [])
                    .filter(
                        (conflict) =>
                            !isImportSpaceConflict(conflict) &&
                            conflict.code !== 'OPAQUE_SEQUENCE_DECISION_REQUIRED' &&
                            !(placementNodeIds.has(conflict.nodeId) && conflict.code === 'SFS_NAME_CONFLICT') &&
                            !(
                                renameConflictCodes.has(conflict.code) &&
                                renameActions.some((action) => action.nodeId === conflict.nodeId)
                            ),
                    )
                    .map((conflict) => [`${conflict.code}:${conflict.nodeId}:${conflict.message}`, conflict]),
            ).values(),
        ),
    );
    const undecidedOpaqueSequences = $derived(
        (plan?.opaqueSequences ?? []).filter(
            (sequence) => !(opaqueSequenceActions[sequence.nodeId] ?? sequence.action),
        ),
    );
    const visibleConflictCount = $derived(
        placementIssues.length + renameActions.length + nonRenameConflicts.length + undecidedOpaqueSequences.length,
    );
    const insertedObjects = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.insertedObjectCount, 0),
    );
    const reusedObjects = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.reusedObjectCount, 0),
    );
    const allocatedBytes = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.additionalAllocatedBytes, 0),
    );
    const visibleWarnings = $derived(
        (plan?.warnings ?? []).filter(
            (warning) =>
                warning.code !== 'OPAQUE_SEQUENCE_PRESERVED_UNCHANGED' && warning.code !== 'OPAQUE_SEQUENCE_SKIPPED',
        ),
    );

    function objectTypeLabel(type: string): string {
        if (type === 'PROG') return 'Program';
        if (type === 'SBAC') return 'Sample Bank';
        if (type === 'SBNK') return 'Sample';
        if (type === 'SMPL') return 'Wave Data';
        if (type === 'SEQU') return 'Sequence';
        if (type === 'VOLUME') return 'Volume';
        return type;
    }

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

    function placementHasChanges(placement: ImageSessionPackageImportPlan['programSlotPlacements'][number]): boolean {
        return placement.mappings.some(
            (mapping) => (programSlots[mapping.nodeId] ?? mapping.destinationSlot) !== mapping.destinationSlot,
        );
    }
</script>

<section class="package-plan-section" aria-label="Import plan">
    <div class="package-section-heading">
        <h3>Import into {targetName}</h3>
        {#if busy}<small>Checking…</small>{/if}
    </div>
    {#if plan}
        <ImportCapacityIssues groups={capacityGroups} />
        {#if !capacityGroups.length}
            <dl class="package-plan-summary">
                <div>
                    <dt>Insert</dt>
                    <dd>{insertedObjects}</dd>
                </div>
                <div>
                    <dt>Reuse</dt>
                    <dd>{reusedObjects}</dd>
                </div>
                {#if plan.valid}
                    <div>
                        <dt>Image space</dt>
                        <dd>{formatStoredSize(allocatedBytes)}</dd>
                    </div>
                {/if}
            </dl>
        {/if}
        {#if plan.opaqueSequences.length > 0}
            <div class="opaque-sequence-choices" aria-label="Undecodable Sequences">
                {#each plan.opaqueSequences as sequence (`${sequence.packageIndex}:${sequence.nodeId}`)}
                    <fieldset>
                        <legend>Sequence “{sequence.name || 'Unnamed'}” could not be decoded</legend>
                        <p>
                            Its event data may be malformed or use an unsupported encoding. Choose how this Sequence
                            should be handled.
                        </p>
                        <label>
                            <input
                                type="radio"
                                name={`opaque-sequence-${sequence.nodeId}`}
                                value="PRESERVE_UNCHANGED"
                                checked={(opaqueSequenceActions[sequence.nodeId] ?? sequence.action) ===
                                    'PRESERVE_UNCHANGED'}
                                disabled={busy}
                                onchange={() => onopaquesequenceaction(sequence.nodeId, 'PRESERVE_UNCHANGED')}
                            />
                            <span>
                                <strong>Preserve unchanged</strong>
                                <small>Import its original event bytes. Playback and editing cannot be verified.</small>
                            </span>
                        </label>
                        <label>
                            <input
                                type="radio"
                                name={`opaque-sequence-${sequence.nodeId}`}
                                value="SKIP"
                                checked={(opaqueSequenceActions[sequence.nodeId] ?? sequence.action) === 'SKIP'}
                                disabled={busy}
                                onchange={() => onopaquesequenceaction(sequence.nodeId, 'SKIP')}
                            />
                            <span>
                                <strong>Skip Sequence</strong>
                                <small>Do not import this Sequence. Other package objects are unaffected.</small>
                            </span>
                        </label>
                    </fieldset>
                {/each}
            </div>
        {/if}
        {#if plan.programSlotPlacements.length > 0}
            <div class="program-slot-placements" aria-label="Program slot placement">
                {#each plan.programSlotPlacements as placement (placement.placementId)}
                    <section
                        class:program-slot-placement-pending={!placement.applied || placementHasChanges(placement)}
                    >
                        <div class="program-slot-placement-heading">
                            <strong>Program slots</strong>
                            {#if placement.mode === 'UNAVAILABLE'}
                                <small>Not enough free slots</small>
                            {:else if placementHasChanges(placement)}
                                <small>Needs check</small>
                            {:else if placement.applied}
                                <small>Checked</small>
                            {:else}
                                <small>Suggested</small>
                            {/if}
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
                                {placement.requiredSlotCount} slots are required, but only
                                {placement.availableSlotCount} are available.
                            </p>
                        {:else if placement.mode === 'CONTIGUOUS'}
                            <label class="program-slot-start">
                                <span>Destination start</span>
                                <input
                                    type="number"
                                    min="1"
                                    max={128 - placement.requiredSlotCount + 1}
                                    disabled={busy}
                                    value={programSlots[placement.mappings[0]?.nodeId] ??
                                        placement.suggestedStartSlot ??
                                        1}
                                    oninput={(event) =>
                                        onprogramstart(placement.placementId, event.currentTarget.valueAsNumber)}
                                />
                            </label>
                        {:else}
                            {#each placement.mappings as mapping (mapping.nodeId)}
                                <label class="program-slot-exception">
                                    <span>Program {formatSlot(mapping.sourceSlot)}</span>
                                    <input
                                        type="number"
                                        min="1"
                                        max="128"
                                        disabled={busy}
                                        value={programSlots[mapping.nodeId] ?? mapping.destinationSlot}
                                        oninput={(event) =>
                                            onprogramslot(mapping.nodeId, event.currentTarget.valueAsNumber)}
                                    />
                                </label>
                            {/each}
                        {/if}
                    </section>
                {/each}
            </div>
        {/if}
        {#if visibleConflictCount > 0}
            <div class="package-conflicts" role="alert">
                <strong
                    >{visibleConflictCount} issue{visibleConflictCount === 1 ? '' : 's'}
                    {visibleConflictCount === 1 ? 'prevents' : 'prevent'} import</strong
                >
                {#each nonRenameConflicts as conflict (`${conflict.code}:${conflict.nodeId}:${conflict.message}`)}
                    <p>{conflict.message}</p>
                {/each}
                {#if renameActions.length > 0}<small>Choose unused destination names.</small>{/if}
                {#each renameActions as action (action.actionId)}
                    <label>
                        <span>{objectTypeLabel(action.objectType)} · {action.sourceName}</span>
                        <input
                            value={renames[action.nodeId] ?? action.destinationName}
                            maxlength="16"
                            disabled={busy}
                            oninput={(event) => onrename(action.nodeId, event.currentTarget.value)}
                        />
                    </label>
                {/each}
            </div>
        {/if}
        {#if plan.programAssignmentAdjustments.length > 0}
            <div class="package-adjustments" aria-label="Program assignment adjustments">
                <strong
                    >{plan.programAssignmentAdjustments.length} unresolved Program assignment{plan
                        .programAssignmentAdjustments.length === 1
                        ? ''
                        : 's'} will be cleared</strong
                >
                {#each plan.programAssignmentAdjustments as adjustment (adjustment.adjustmentId)}
                    <p>
                        <span>{adjustment.programName || `Program ${adjustment.programSlot}`}</span>
                        <small
                            >{adjustment.targetObjectType === 'SBAC' ? 'Sample Bank' : 'Sample'}
                            “{adjustment.targetName}” ·
                            {adjustment.origin === 'EXISTING_PROGRAM' ? 'existing Program' : 'imported Program'}</small
                        >
                    </p>
                {/each}
            </div>
        {/if}
        {#each visibleWarnings as warning}
            <p class="package-warning">
                {warning.origin === 'TARGET' && warning.objectType === 'SEQU'
                    ? `Existing Sequence “${warning.objectName || 'Unnamed'}”${warning.volumeName ? ` in ${warning.volumeName}` : ''} could not be decoded. It is unrelated to this import and will be preserved unchanged.`
                    : warning.message}
            </p>
        {/each}
    {/if}
</section>
