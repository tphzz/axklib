<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { ImageSessionPackageImportPlan, PackageInspection } from '../transport';
    import Icon from './Icon.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';

    interface Props {
        targetName: string;
        desktop: boolean;
        sourceName: string;
        inspection: PackageInspection | null;
        plan: ImageSessionPackageImportPlan | null;
        renames: Record<string, string>;
        programSlots: Record<string, number>;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        progress: number;
        error: string;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
        onchange: () => void;
        onrename: (nodeId: string, name: string) => void;
        onprogramslot: (nodeId: string, slot: number) => void;
        onprogramstart: (placementId: string, start: number) => void;
        onreplan: () => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    interface PackageTreeRow {
        key: string;
        depth: number;
        name: string;
        type: string;
    }

    let {
        targetName,
        desktop,
        sourceName,
        inspection,
        plan,
        renames,
        programSlots,
        status,
        progress,
        error,
        onchooseworkspace,
        onchooselocal,
        onchange,
        onrename,
        onprogramslot,
        onprogramstart,
        onreplan,
        oncancel,
        onconfirm,
    }: Props = $props();

    const busy = $derived(status === 'loading' || status === 'planning' || status === 'applying');
    const locked = $derived(status === 'applying');
    const canImport = $derived(status === 'ready' && Boolean(plan?.valid));
    const treeRows = $derived(packageTree(inspection));
    const conflictNodes = $derived(new Set(plan?.conflicts.map((conflict) => conflict.nodeId) ?? []));
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
    const renameActions = $derived(
        Array.from(
            new Map(
                (plan?.actions ?? [])
                    .filter(
                        (action) =>
                            !placementNodeIds.has(action.nodeId) &&
                            conflictNodes.has(action.nodeId) &&
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
                            !(placementNodeIds.has(conflict.nodeId) && conflict.code === 'SFS_NAME_CONFLICT') &&
                            !renameActions.some((action) => action.nodeId === conflict.nodeId),
                    )
                    .map((conflict) => [`${conflict.code}:${conflict.nodeId}:${conflict.message}`, conflict]),
            ).values(),
        ),
    );
    const visibleConflictCount = $derived(placementIssues.length + renameActions.length + nonRenameConflicts.length);
    const insertedObjects = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.insertedObjectCount, 0),
    );
    const reusedObjects = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.reusedObjectCount, 0),
    );
    const allocatedBytes = $derived(
        (plan?.allocation ?? []).reduce((total, allocation) => total + allocation.additionalAllocatedBytes, 0),
    );

    function objectTypeLabel(type: string): string {
        if (type === 'PROG') return 'Program';
        if (type === 'SBAC') return 'Sample Bank';
        if (type === 'SBNK') return 'Sample';
        if (type === 'SMPL') return 'Wave Data';
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

    function packageTree(value: PackageInspection | null): PackageTreeRow[] {
        if (!value) return [];
        const objects = new Map(value.objects.map((object) => [object.nodeId, object]));
        const children = new Map<string, string[]>();
        for (const relationship of value.relationships) {
            const targets = children.get(relationship.sourceNodeId) ?? [];
            if (!targets.includes(relationship.targetNodeId)) targets.push(relationship.targetNodeId);
            children.set(relationship.sourceNodeId, targets);
        }
        const rows: PackageTreeRow[] = [];
        const append = (nodeId: string, depth: number, visited: Set<string>, path: string) => {
            if (visited.has(nodeId)) return;
            const object = objects.get(nodeId);
            if (!object) return;
            rows.push({ key: `${path}:${nodeId}`, depth, name: object.name, type: object.objectType });
            const nextVisited = new Set(visited).add(nodeId);
            for (const [childIndex, child] of (children.get(nodeId) ?? []).entries()) {
                append(child, depth + 1, nextVisited, `${path}.${childIndex}`);
            }
        };
        value.roots.forEach((root, rootIndex) => {
            rows.push({
                key: `root-${rootIndex}`,
                depth: 0,
                name: root.displayName,
                type: objectTypeLabel(root.kind),
            });
            root.nodeIds.forEach((nodeId, nodeIndex) => append(nodeId, 1, new Set(), `root-${rootIndex}.${nodeIndex}`));
        });
        return rows;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide package-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import axklib package"
        aria-busy={busy}
        use:modal={{ onescape: locked ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2>Import package</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={locked} onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            {#if !sourceName}
                <ImportSourceChoice
                    label="Package source"
                    heading="Choose a package"
                    description={`Import into ${targetName} from a configured storage location.`}
                    workspaceDetail="Choose from a configured workspace"
                    computerDetail="Choose a local package and upload it"
                    computerAvailable={desktop}
                    {onchooseworkspace}
                    {onchooselocal}
                />
            {:else}
                <section class="package-source-summary" aria-label="Selected package">
                    <div>
                        <small>Package</small>
                        <strong>{sourceName}</strong>
                    </div>
                    <button class="secondary-button" type="button" disabled={locked} onclick={onchange}>Change</button>
                </section>

                {#if status === 'loading'}
                    <p class="dialog-progress" role="status">
                        {progress > 0 ? `Uploading package · ${Math.round(progress * 100)}%` : 'Verifying package…'}
                    </p>
                {:else if inspection}
                    <div class="package-review-grid">
                        <section class="package-tree-section" aria-label="Package contents">
                            <div class="package-section-heading">
                                <h3>Package contents</h3>
                                <small
                                    >{inspection.objects.length} objects ·
                                    {formatStoredSize(inspection.totalPayloadBytes)}</small
                                >
                            </div>
                            <div class="package-tree" role="tree">
                                {#each treeRows as row (row.key)}
                                    <div
                                        class="package-tree-row"
                                        role="treeitem"
                                        aria-selected="false"
                                        style={`--package-depth: ${row.depth}`}
                                    >
                                        <Icon
                                            name={row.type === 'Wave Data' || row.type === 'SMPL'
                                                ? 'waveform'
                                                : row.depth === 0
                                                  ? 'folder'
                                                  : 'archive'}
                                            size={13}
                                        />
                                        <strong>{row.name}</strong>
                                        <small>{objectTypeLabel(row.type)}</small>
                                    </div>
                                {/each}
                            </div>
                        </section>

                        <section class="package-plan-section" aria-label="Import plan">
                            <div class="package-section-heading">
                                <h3>Import into {targetName}</h3>
                                {#if status === 'planning'}<small>Checking…</small>{/if}
                            </div>
                            {#if plan}
                                <dl class="package-plan-summary">
                                    <div>
                                        <dt>Insert</dt>
                                        <dd>{insertedObjects}</dd>
                                    </div>
                                    <div>
                                        <dt>Reuse</dt>
                                        <dd>{reusedObjects}</dd>
                                    </div>
                                    <div>
                                        <dt>Image space</dt>
                                        <dd>{formatStoredSize(allocatedBytes)}</dd>
                                    </div>
                                </dl>
                                {#if plan.programSlotPlacements.length > 0}
                                    <div class="program-slot-placements" aria-label="Program slot placement">
                                        {#each plan.programSlotPlacements as placement (placement.placementId)}
                                            <section class:program-slot-placement-pending={!placement.applied}>
                                                <div class="program-slot-placement-heading">
                                                    <strong>Program slots</strong>
                                                    {#if placement.mode === 'UNAVAILABLE'}
                                                        <small>Not enough free slots</small>
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
                                                {:else if !placement.applied && placement.mode === 'CONTIGUOUS'}
                                                    <label class="program-slot-start">
                                                        <span>Destination start</span>
                                                        <input
                                                            type="number"
                                                            min="1"
                                                            max={128 - placement.requiredSlotCount + 1}
                                                            value={programSlots[placement.mappings[0]?.nodeId] ??
                                                                placement.suggestedStartSlot ??
                                                                1}
                                                            oninput={(event) =>
                                                                onprogramstart(
                                                                    placement.placementId,
                                                                    event.currentTarget.valueAsNumber,
                                                                )}
                                                        />
                                                    </label>
                                                {:else if !placement.applied}
                                                    <small>Using the lowest available Program slots.</small>
                                                {/if}
                                                {#each placement.mappings.filter((mapping) => mapping.requiresUserAction) as mapping (mapping.nodeId)}
                                                    <label class="program-slot-exception">
                                                        <span>Program {formatSlot(mapping.sourceSlot)}</span>
                                                        <input
                                                            type="number"
                                                            min="1"
                                                            max="128"
                                                            value={programSlots[mapping.nodeId] ??
                                                                mapping.destinationSlot}
                                                            oninput={(event) =>
                                                                onprogramslot(
                                                                    mapping.nodeId,
                                                                    event.currentTarget.valueAsNumber,
                                                                )}
                                                        />
                                                    </label>
                                                {/each}
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
                                        {#if renameActions.length > 0}<small>Choose unused destination names.</small
                                            >{/if}
                                        {#each renameActions as action (action.actionId)}
                                            <label>
                                                <span>{objectTypeLabel(action.objectType)} · {action.sourceName}</span>
                                                <input
                                                    value={renames[action.nodeId] ?? action.destinationName}
                                                    maxlength="16"
                                                    oninput={(event) =>
                                                        onrename(action.nodeId, event.currentTarget.value)}
                                                />
                                            </label>
                                        {/each}
                                        {#if renameActions.length > 0 || placementIssues.length > 0}
                                            <button
                                                class="secondary-button"
                                                type="button"
                                                disabled={busy}
                                                onclick={onreplan}>Check names</button
                                            >
                                        {/if}
                                    </div>
                                {:else}
                                    <p class="package-plan-ready">
                                        <Icon name="check" size={14} /> Ready to import
                                    </p>
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
                                                <span
                                                    >{adjustment.programName ||
                                                        `Program ${adjustment.programSlot}`}</span
                                                >
                                                <small
                                                    >{adjustment.targetObjectType === 'SBAC' ? 'Sample Bank' : 'Sample'}
                                                    “{adjustment.targetName}” ·
                                                    {adjustment.origin === 'EXISTING_PROGRAM'
                                                        ? 'existing Program'
                                                        : 'imported Program'}</small
                                                >
                                            </p>
                                        {/each}
                                    </div>
                                {/if}
                                {#each plan.warnings as warning}
                                    <p class="package-warning">{warning.message}</p>
                                {/each}
                            {:else}
                                <p class="dialog-progress" role="status">Planning import…</p>
                            {/if}
                        </section>
                    </div>
                {/if}
            {/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={locked} onclick={oncancel}>Cancel</button>
            {#if sourceName}
                <button class="primary-button" type="button" disabled={!canImport} onclick={onconfirm}>
                    {status === 'applying' ? 'Importing…' : 'Import package'}
                </button>
            {/if}
        </footer>
    </div>
</div>
