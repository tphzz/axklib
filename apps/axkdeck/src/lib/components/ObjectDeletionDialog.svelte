<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type {
        ObjectDeletionImpact,
        ObjectDeletionInspection,
        ObjectDeletionNotice,
        ObjectDeletionReference,
    } from '../transport';
    import Icon from './Icon.svelte';

    interface Props {
        inspection: ObjectDeletionInspection | null;
        loading: boolean;
        busy: boolean;
        error: string;
        onselectionchange: (objectId: string, selected: boolean) => void;
        onselectall: (selected: boolean) => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let { inspection, loading, busy, error, onselectionchange, onselectall, oncancel, onconfirm }: Props = $props();
    let selectAllCheckbox = $state<HTMLInputElement>();
    const maximumDeletionObjects = 1024;

    const selectedIds = $derived(new Set(inspection?.selectedObjectIds ?? []));
    const impactsById = $derived(new Map(inspection?.impacts.map((impact) => [impact.objectId, impact]) ?? []));
    const targetImpacts = $derived(inspection?.impacts.filter((impact) => impact.role === 'TARGET') ?? []);
    const eligibleTargets = $derived(targetImpacts.filter((impact) => impact.status === 'REQUIRED'));
    const blockedTargets = $derived(targetImpacts.filter((impact) => impact.status === 'BLOCKED'));
    const optionalImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL') ?? [],
    );
    const retainedImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'PRESERVED') ?? [],
    );
    const selectedOptionalCount = $derived(optionalImpacts.filter((impact) => selectedIds.has(impact.objectId)).length);
    const selectedObjectCount = $derived(inspection?.selectedObjectIds.length ?? 0);
    const cleanupCapacity = $derived(
        Math.max(0, maximumDeletionObjects - (inspection?.targetObjectIds.length ?? maximumDeletionObjects)),
    );
    const selectableCleanupCount = $derived(Math.min(optionalImpacts.length, cleanupCapacity));
    const allOptionalSelected = $derived(
        selectableCleanupCount > 0 && selectedOptionalCount === selectableCleanupCount,
    );
    const someOptionalSelected = $derived(selectedOptionalCount > 0 && !allOptionalSelected);
    const selectionLimitReached = $derived(
        (inspection?.targetObjectIds.length ?? 0) + selectedOptionalCount >= maximumDeletionObjects &&
            selectedOptionalCount < optionalImpacts.length,
    );
    const visibleWarnings = $derived(
        inspection?.warnings.filter((warning) => warning.code !== 'WAVE_DATA_WILL_BE_UNREFERENCED') ?? [],
    );
    const canConfirm = $derived(Boolean(inspection?.canApply) && !loading && !busy);
    const title = $derived(
        targetImpacts.length === 1
            ? `Delete ${objectTypeLabel(targetImpacts[0]?.objectType)}`
            : `Delete ${targetImpacts.length || ''} objects`.replace('  ', ' '),
    );
    const summary = $derived(
        targetImpacts.length === 1
            ? `Review deletion of “${targetImpacts[0]?.objectName ?? ''}”`
            : `Review deletion of ${targetImpacts.length} selected objects`,
    );
    const deletionButtonLabel = $derived(
        busy
            ? 'Deleting…'
            : blockedTargets.length > 0
              ? `Delete ${selectedObjectCount} eligible ${selectedObjectCount === 1 ? 'object' : 'objects'}`
              : `Delete ${selectedObjectCount} ${selectedObjectCount === 1 ? 'object' : 'objects'}`,
    );

    $effect(() => {
        if (selectAllCheckbox) selectAllCheckbox.indeterminate = someOptionalSelected;
    });

    function objectTypeLabel(objectType: string | null | undefined): string {
        if (objectType === 'PROG') return 'Program';
        if (objectType === 'SBAC') return 'Sample Bank';
        if (objectType === 'SBNK') return 'Sample';
        if (objectType === 'SMPL') return 'Wave Data';
        return 'object';
    }

    function prerequisiteMissing(impact: ObjectDeletionImpact): boolean {
        return impact.prerequisiteObjectIds.some((objectId) => !selectedIds.has(objectId));
    }

    function optionalDepth(impact: ObjectDeletionImpact, visited = new Set<string>()): number {
        if (visited.has(impact.objectId)) return 0;
        const nextVisited = new Set(visited).add(impact.objectId);
        const prerequisites = impact.prerequisiteObjectIds
            .map((objectId) => impactsById.get(objectId))
            .filter(
                (candidate): candidate is ObjectDeletionImpact =>
                    candidate !== undefined && candidate.role === 'DEPENDENCY' && candidate.status === 'OPTIONAL',
            );
        return prerequisites.length === 0
            ? 0
            : 1 + Math.max(...prerequisites.map((candidate) => optionalDepth(candidate, nextVisited)));
    }

    function missingPrerequisiteNames(impact: ObjectDeletionImpact): string[] {
        return impact.prerequisiteObjectIds
            .filter((objectId) => !selectedIds.has(objectId))
            .map((objectId) => impactsById.get(objectId)?.objectName ?? 'related object');
    }

    function joinNames(names: string[]): string {
        if (names.length <= 1) return names[0] ?? '';
        if (names.length === 2) return `${names[0]} and ${names[1]}`;
        return `${names.slice(0, -1).join(', ')}, and ${names.at(-1)}`;
    }

    function referencesFor(impact: ObjectDeletionImpact): ObjectDeletionReference[] {
        return (
            inspection?.references.filter(
                (reference) => reference.effect !== 'REMOVED' && reference.targetObjectId === impact.objectId,
            ) ?? []
        );
    }

    function noticesFor(impact: ObjectDeletionImpact): ObjectDeletionNotice[] {
        return inspection?.blockers.filter((notice) => notice.objectIds.includes(impact.objectId)) ?? [];
    }

    function referenceLabel(reference: ObjectDeletionReference): string {
        return `Referenced by ${objectTypeLabel(reference.sourceObjectType)} ${reference.sourceObjectName}`.trim();
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide object-deletion-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        aria-busy={loading || busy}
        use:modal={{ onescape: busy ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <h2>{title}</h2>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="object-deletion-content">
            <div class="destructive-summary">
                <strong>{summary}</strong>
                <p>This permanently modifies the image; undo is not available.</p>
            </div>

            {#if loading && !inspection}
                <p class="dialog-progress" role="status">Inspecting references and allocation…</p>
            {:else if inspection}
                {#if visibleWarnings.length > 0}
                    <section class="deletion-notices" aria-label="Deletion warnings">
                        <h3>Warnings</h3>
                        {#each visibleWarnings as warning (`${warning.code}\0${warning.objectIds.join('\0')}`)}
                            <p>{warning.message}</p>
                        {/each}
                    </section>
                {/if}

                {#if eligibleTargets.length > 0}
                    <section class="deletion-impact-section" aria-label="Will be deleted">
                        <h3>Will be deleted</h3>
                        <div class="deletion-impact-list">
                            {#each eligibleTargets as impact (impact.objectId)}
                                <div class="deletion-impact deletion-impact-static">
                                    <span class="deletion-impact-icon" aria-hidden="true">
                                        <Icon name="trash" size={14} />
                                    </span>
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                        </span>
                                        <small
                                            >{impact.volumeName || 'No volume'} · {formatStoredSize(
                                                impact.storedSizeBytes,
                                            )}</small
                                        >
                                        {#each referencesFor(impact) as reference (`${reference.sourceObjectId}\0${reference.type}\0${reference.targetObjectId ?? ''}`)}
                                            <small class="deletion-reference">{referenceLabel(reference)}</small>
                                        {/each}
                                    </span>
                                </div>
                            {/each}
                        </div>
                    </section>
                {/if}

                {#if blockedTargets.length > 0}
                    <section class="deletion-impact-section deletion-blockers" aria-label="Cannot be deleted">
                        <h3>Cannot be deleted</h3>
                        <p class="deletion-section-help">
                            These selected objects will remain. Other eligible selections can still be deleted.
                        </p>
                        <div class="deletion-impact-list">
                            {#each blockedTargets as impact (impact.objectId)}
                                <div class="blocked deletion-impact deletion-impact-static">
                                    <span class="deletion-impact-icon deletion-impact-icon-muted" aria-hidden="true">
                                        <Icon name="lock" size={14} />
                                    </span>
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                        </span>
                                        {#if impact.reason}<small>{impact.reason}</small>{/if}
                                        {#each noticesFor(impact) as notice (`${notice.code}\0${notice.objectIds.join('\0')}`)}
                                            <small>{notice.message}</small>
                                        {/each}
                                        {#each referencesFor(impact) as reference (`${reference.sourceObjectId}\0${reference.type}\0${reference.targetObjectId ?? ''}`)}
                                            <small class="deletion-reference">{referenceLabel(reference)}</small>
                                        {/each}
                                    </span>
                                </div>
                            {/each}
                        </div>
                    </section>
                {/if}

                {#if optionalImpacts.length > 0}
                    <section class="deletion-impact-section deletion-optional-section" aria-label="Optional cleanup">
                        <h3>Optional cleanup</h3>
                        <p class="deletion-section-help">
                            These related objects remain valid if kept. Select any you also want to delete.
                        </p>
                        <label class="deletion-select-all">
                            <input
                                bind:this={selectAllCheckbox}
                                class="deletion-checkbox"
                                type="checkbox"
                                checked={allOptionalSelected}
                                disabled={loading || busy}
                                onchange={(event) => onselectall(event.currentTarget.checked)}
                            />
                            <span>
                                {optionalImpacts.length > cleanupCapacity
                                    ? `Also delete up to ${selectableCleanupCount} of ${optionalImpacts.length}`
                                    : `Also delete all (${optionalImpacts.length})`}
                            </span>
                        </label>
                        <div class="deletion-impact-list">
                            {#each optionalImpacts as impact (impact.objectId)}
                                {@const disabled =
                                    loading ||
                                    busy ||
                                    prerequisiteMissing(impact) ||
                                    (selectionLimitReached && !impact.selected)}
                                {@const missingNames = missingPrerequisiteNames(impact)}
                                {@const depth = optionalDepth(impact)}
                                <label
                                    class:nested={depth > 0}
                                    class:disabled
                                    class="deletion-impact deletion-impact-selectable"
                                    style={`--deletion-depth: ${depth}`}
                                >
                                    <input
                                        class="deletion-checkbox"
                                        type="checkbox"
                                        checked={impact.selected}
                                        {disabled}
                                        aria-label={`Delete ${objectTypeLabel(impact.objectType)} ${impact.objectName}`}
                                        onchange={(event) =>
                                            onselectionchange(impact.objectId, event.currentTarget.checked)}
                                    />
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                            <span class="deletion-outcome"
                                                >{impact.selected ? 'Will delete' : 'Keep'}</span
                                            >
                                        </span>
                                        <small>
                                            {missingNames.length > 0
                                                ? `Available after deleting ${joinNames(missingNames)}`
                                                : impact.reason}
                                        </small>
                                        <small
                                            >{impact.volumeName || 'No volume'} · {formatStoredSize(
                                                impact.storedSizeBytes,
                                            )}</small
                                        >
                                    </span>
                                </label>
                            {/each}
                        </div>
                    </section>
                {/if}

                {#if retainedImpacts.length > 0}
                    <section class="deletion-impact-section" aria-label="Will remain">
                        <h3>Will remain</h3>
                        <div class="deletion-impact-list">
                            {#each retainedImpacts as impact (impact.objectId)}
                                <div class="deletion-impact deletion-impact-static">
                                    <span class="deletion-impact-icon deletion-impact-icon-muted" aria-hidden="true">
                                        <Icon name="lock" size={14} />
                                    </span>
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                        </span>
                                        <small>{impact.reason}</small>
                                        <small
                                            >{impact.volumeName || 'No volume'} · {formatStoredSize(
                                                impact.storedSizeBytes,
                                            )}</small
                                        >
                                        {#each referencesFor(impact) as reference (`${reference.sourceObjectId}\0${reference.type}\0${reference.targetObjectId ?? ''}`)}
                                            <small class="deletion-reference">{referenceLabel(reference)}</small>
                                        {/each}
                                    </span>
                                </div>
                            {/each}
                        </div>
                    </section>
                {/if}
            {/if}

            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer object-deletion-footer">
            <p class="deletion-footer-summary" aria-live="polite">
                {#if inspection?.canApply}
                    <strong
                        >{selectedObjectCount}
                        {selectedObjectCount === 1 ? 'object' : 'objects'} will be deleted</strong
                    >
                    <span>
                        {formatStoredSize(inspection.estimatedFreedBytes)} freed ({inspection.estimatedFreedClusters}
                        {inspection.estimatedFreedClusters === 1 ? ' cluster' : ' clusters'})
                        {#if blockedTargets.length > 0}
                            · {blockedTargets.length} selected {blockedTargets.length === 1
                                ? 'object remains'
                                : 'objects remain'}
                        {/if}
                    </span>
                {:else if inspection}
                    <strong>No selected object can be deleted</strong>
                    <span>Review the constraints above.</span>
                {:else}
                    <strong>Inspecting deletion</strong>
                    <span>Calculating affected objects and storage.</span>
                {/if}
            </p>
            <div class="object-deletion-actions">
                <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
                <button class="danger-button" type="button" disabled={!canConfirm} onclick={onconfirm}>
                    {deletionButtonLabel}
                </button>
            </div>
        </footer>
    </div>
</div>
