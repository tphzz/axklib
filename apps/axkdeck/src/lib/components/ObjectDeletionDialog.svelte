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
        onselectionchange: (role: 'REFERRER' | 'DEPENDENCY', objectId: string, selected: boolean) => void;
        onselectall: (role: 'REFERRER' | 'DEPENDENCY', selected: boolean) => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let { inspection, loading, busy, error, onselectionchange, onselectall, oncancel, onconfirm }: Props = $props();
    let selectAllReferrersCheckbox = $state<HTMLInputElement>();
    let selectAllCleanupCheckbox = $state<HTMLInputElement>();
    const maximumDeletionObjects = 1024;

    const selectedIds = $derived(new Set(inspection?.selectedObjectIds ?? []));
    const impactsById = $derived(new Map(inspection?.impacts.map((impact) => [impact.objectId, impact]) ?? []));
    const targetImpacts = $derived(inspection?.impacts.filter((impact) => impact.role === 'TARGET') ?? []);
    const eligibleTargets = $derived(targetImpacts.filter((impact) => impact.status === 'REQUIRED'));
    const blockedTargets = $derived(targetImpacts.filter((impact) => impact.status === 'BLOCKED'));
    const referenceBlockedTargets = $derived(
        blockedTargets.filter((impact) => impact.reason === 'References must be resolved'),
    );
    const unsafeBlockedTargets = $derived(
        blockedTargets.filter((impact) => impact.reason !== 'References must be resolved'),
    );
    const referrerImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'REFERRER' && impact.status === 'OPTIONAL') ?? [],
    );
    const optionalImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL') ?? [],
    );
    const retainedImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'PRESERVED') ?? [],
    );
    const requestedReferrerCount = $derived(referrerImpacts.filter((impact) => impact.requested).length);
    const requestedCleanupCount = $derived(optionalImpacts.filter((impact) => impact.requested).length);
    const selectedObjectCount = $derived(inspection?.selectedObjectIds.length ?? 0);
    const referrerCapacity = $derived(
        Math.max(
            0,
            maximumDeletionObjects -
                (inspection?.targetObjectIds.length ?? maximumDeletionObjects) -
                (inspection?.cleanupObjectIds.length ?? 0),
        ),
    );
    const cleanupCapacity = $derived(
        Math.max(
            0,
            maximumDeletionObjects -
                (inspection?.targetObjectIds.length ?? maximumDeletionObjects) -
                (inspection?.referrerObjectIds.length ?? 0),
        ),
    );
    const selectableReferrerCount = $derived(Math.min(referrerImpacts.length, referrerCapacity));
    const selectableCleanupCount = $derived(Math.min(optionalImpacts.length, cleanupCapacity));
    const allReferrersRequested = $derived(
        selectableReferrerCount > 0 && requestedReferrerCount === selectableReferrerCount,
    );
    const someReferrersRequested = $derived(requestedReferrerCount > 0 && !allReferrersRequested);
    const allCleanupRequested = $derived(
        selectableCleanupCount > 0 && requestedCleanupCount === selectableCleanupCount,
    );
    const someCleanupRequested = $derived(requestedCleanupCount > 0 && !allCleanupRequested);
    const selectionLimitReached = $derived(
        (inspection?.targetObjectIds.length ?? 0) + requestedReferrerCount + requestedCleanupCount >=
            maximumDeletionObjects,
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
        if (selectAllReferrersCheckbox) selectAllReferrersCheckbox.indeterminate = someReferrersRequested;
        if (selectAllCleanupCheckbox) selectAllCleanupCheckbox.indeterminate = someCleanupRequested;
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
                    candidate !== undefined && candidate.role !== 'TARGET' && candidate.status === 'OPTIONAL',
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

                {#if referenceBlockedTargets.length > 0}
                    <section class="deletion-impact-section deletion-blockers" aria-label="References must be resolved">
                        <h3>References must be resolved</h3>
                        <p class="deletion-section-help">
                            Select the complete referencing chain below to make these objects eligible for deletion.
                        </p>
                        <div class="deletion-impact-list">
                            {#each referenceBlockedTargets as impact (impact.objectId)}
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

                {#if unsafeBlockedTargets.length > 0}
                    <section class="deletion-impact-section deletion-blockers" aria-label="Cannot be deleted safely">
                        <h3>Cannot be deleted safely</h3>
                        <p class="deletion-section-help">
                            These selected objects will remain. Other eligible selections can still be deleted.
                        </p>
                        <div class="deletion-impact-list">
                            {#each unsafeBlockedTargets as impact (impact.objectId)}
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

                {#if referrerImpacts.length > 0}
                    <section class="deletion-impact-section deletion-optional-section" aria-label="Referencing objects">
                        <h3>Referencing objects</h3>
                        <p class="deletion-section-help">
                            Delete a complete referencing chain to make the requested objects eligible.
                        </p>
                        <label class="deletion-select-all">
                            <input
                                bind:this={selectAllReferrersCheckbox}
                                class="dialog-checkbox"
                                type="checkbox"
                                checked={allReferrersRequested}
                                disabled={loading || busy}
                                onchange={(event) => onselectall('REFERRER', event.currentTarget.checked)}
                            />
                            <span>
                                {referrerImpacts.length > referrerCapacity
                                    ? `Delete up to ${selectableReferrerCount} of ${referrerImpacts.length} referencing objects`
                                    : `Delete all referencing objects (${referrerImpacts.length})`}
                            </span>
                        </label>
                        <div class="deletion-impact-list">
                            {#each referrerImpacts as impact (impact.objectId)}
                                {@const disabled =
                                    loading ||
                                    busy ||
                                    prerequisiteMissing(impact) ||
                                    (selectionLimitReached && !impact.requested)}
                                {@const missingNames = missingPrerequisiteNames(impact)}
                                {@const depth = optionalDepth(impact)}
                                <label
                                    class:nested={depth > 0}
                                    class:disabled
                                    class="deletion-impact deletion-impact-selectable"
                                    style={`--deletion-depth: ${depth}`}
                                >
                                    <input
                                        class="dialog-checkbox"
                                        type="checkbox"
                                        checked={impact.requested}
                                        {disabled}
                                        aria-label={`Delete referencing ${objectTypeLabel(impact.objectType)} ${impact.objectName}`}
                                        onchange={(event) =>
                                            onselectionchange('REFERRER', impact.objectId, event.currentTarget.checked)}
                                    />
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                            <span class="deletion-outcome">
                                                {impact.requested
                                                    ? impact.selected
                                                        ? 'Will delete'
                                                        : 'Pending'
                                                    : 'Keep'}
                                            </span>
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

                {#if optionalImpacts.length > 0}
                    <section class="deletion-impact-section deletion-optional-section" aria-label="Optional cleanup">
                        <h3>Optional cleanup</h3>
                        <p class="deletion-section-help">
                            These related objects remain valid if kept. Select any you also want to delete.
                        </p>
                        <label class="deletion-select-all">
                            <input
                                bind:this={selectAllCleanupCheckbox}
                                class="dialog-checkbox"
                                type="checkbox"
                                checked={allCleanupRequested}
                                disabled={loading || busy}
                                onchange={(event) => onselectall('DEPENDENCY', event.currentTarget.checked)}
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
                                    (selectionLimitReached && !impact.requested)}
                                {@const missingNames = missingPrerequisiteNames(impact)}
                                {@const depth = optionalDepth(impact)}
                                <label
                                    class:nested={depth > 0}
                                    class:disabled
                                    class="deletion-impact deletion-impact-selectable"
                                    style={`--deletion-depth: ${depth}`}
                                >
                                    <input
                                        class="dialog-checkbox"
                                        type="checkbox"
                                        checked={impact.requested}
                                        {disabled}
                                        aria-label={`Delete ${objectTypeLabel(impact.objectType)} ${impact.objectName}`}
                                        onchange={(event) =>
                                            onselectionchange(
                                                'DEPENDENCY',
                                                impact.objectId,
                                                event.currentTarget.checked,
                                            )}
                                    />
                                    <span class="deletion-impact-copy">
                                        <span class="deletion-impact-heading">
                                            <strong>{impact.objectName}</strong>
                                            <small>{objectTypeLabel(impact.objectType)}</small>
                                            <span class="deletion-outcome">
                                                {impact.requested
                                                    ? impact.selected
                                                        ? 'Will delete'
                                                        : 'Pending'
                                                    : 'Keep'}
                                            </span>
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
