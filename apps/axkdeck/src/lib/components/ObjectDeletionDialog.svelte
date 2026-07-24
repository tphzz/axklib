<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { ObjectDeletionImpact, ObjectDeletionInspection, ObjectDeletionReference } from '../transport';
    import Icon from './Icon.svelte';

    interface Props {
        targetName: string;
        inspection: ObjectDeletionInspection | null;
        loading: boolean;
        busy: boolean;
        error: string;
        onselectionchange: (objectId: string, selected: boolean) => void;
        onselectall: (selected: boolean) => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let { targetName, inspection, loading, busy, error, onselectionchange, onselectall, oncancel, onconfirm }: Props =
        $props();
    let selectAllCheckbox = $state<HTMLInputElement>();

    const selectedIds = $derived(new Set(inspection?.selectedObjectIds ?? []));
    const impactsById = $derived(new Map(inspection?.impacts.map((impact) => [impact.objectId, impact]) ?? []));
    const targetImpact = $derived(inspection?.impacts.find((impact) => impact.role === 'TARGET'));
    const optionalImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL') ?? [],
    );
    const retainedImpacts = $derived(
        inspection?.impacts.filter((impact) => impact.role === 'DEPENDENCY' && impact.status === 'PRESERVED') ?? [],
    );
    const selectedOptionalCount = $derived(optionalImpacts.filter((impact) => selectedIds.has(impact.objectId)).length);
    const selectedObjectCount = $derived((targetImpact ? 1 : 0) + selectedOptionalCount);
    const allOptionalSelected = $derived(
        optionalImpacts.length > 0 && selectedOptionalCount === optionalImpacts.length,
    );
    const someOptionalSelected = $derived(selectedOptionalCount > 0 && !allOptionalSelected);
    const visibleWarnings = $derived(
        inspection?.warnings.filter((warning) => warning.code !== 'WAVE_DATA_WILL_BE_UNREFERENCED') ?? [],
    );
    const canConfirm = $derived(Boolean(inspection?.valid) && !loading && !busy);
    const title = $derived(`Delete ${objectTypeLabel(targetImpact?.objectType)}`);
    const targetSectionTitle = $derived(inspection?.valid ? 'Will be deleted' : 'Requested deletion');
    const deletionButtonLabel = $derived(
        busy
            ? 'Deleting…'
            : selectedObjectCount === 0
              ? 'Delete'
              : `Delete ${selectedObjectCount} ${selectedObjectCount === 1 ? 'object' : 'objects'}`,
    );

    $effect(() => {
        if (selectAllCheckbox) selectAllCheckbox.indeterminate = someOptionalSelected;
    });

    function objectTypeLabel(objectType: string | null | undefined): string {
        if (objectType === 'SBAC') return 'Sample Bank';
        if (objectType === 'SBNK') return 'Sample';
        if (objectType === 'SMPL') return 'Wave Data';
        if (objectType === 'PROG') return 'Program';
        if (objectType === 'SEQU') return 'Sequence';
        if (objectType === 'PRF3') return 'Profile';
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
        if (prerequisites.length === 0) return 0;
        return 1 + Math.max(...prerequisites.map((candidate) => optionalDepth(candidate, nextVisited)));
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

    function referenceLabel(reference: ObjectDeletionReference): string {
        const source = `${objectTypeLabel(reference.sourceObjectType)} ${reference.sourceObjectName}`.trim();
        return `Referenced by ${source}`;
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
                <strong>
                    {inspection?.valid
                        ? `Review deletion of “${targetName}”`
                        : `Deletion of “${targetName}” is blocked`}
                </strong>
                <p>This permanently modifies the image; undo is not available.</p>
            </div>

            {#if loading && !inspection}
                <p class="dialog-progress" role="status">Inspecting references and allocation…</p>
            {:else if inspection && targetImpact}
                {#if inspection.blockers.length > 0}
                    <section class="deletion-notices deletion-blockers" aria-label="Deletion blockers">
                        <h3>Cannot delete</h3>
                        {#each inspection.blockers as blocker}
                            <p>{blocker.message}</p>
                        {/each}
                    </section>
                {/if}

                {#if visibleWarnings.length > 0}
                    <section class="deletion-notices" aria-label="Deletion warnings">
                        <h3>Warnings</h3>
                        {#each visibleWarnings as warning}
                            <p>{warning.message}</p>
                        {/each}
                    </section>
                {/if}

                <section class="deletion-impact-section" aria-label={targetSectionTitle}>
                    <h3>{targetSectionTitle}</h3>
                    <div class="deletion-impact-list">
                        <div class:blocked={!inspection.valid} class="deletion-impact deletion-impact-static">
                            <span class="deletion-impact-icon" aria-hidden="true">
                                <Icon name="trash" size={14} />
                            </span>
                            <span class="deletion-impact-copy">
                                <span class="deletion-impact-heading">
                                    <strong>{targetImpact.objectName}</strong>
                                    <small>{objectTypeLabel(targetImpact.objectType)}</small>
                                </span>
                                <small
                                    >{targetImpact.volumeName || 'No volume'} · {formatStoredSize(
                                        targetImpact.storedSizeBytes,
                                    )}</small
                                >
                                {#each referencesFor(targetImpact) as reference}
                                    <small class="deletion-reference">{referenceLabel(reference)}</small>
                                {/each}
                            </span>
                        </div>
                    </div>
                </section>

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
                            <span>Also delete all ({optionalImpacts.length})</span>
                        </label>
                        <div class="deletion-impact-list">
                            {#each optionalImpacts as impact (impact.objectId)}
                                {@const disabled = loading || busy || prerequisiteMissing(impact)}
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
                                        {#each referencesFor(impact) as reference}
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
                {#if inspection?.valid}
                    <strong
                        >{selectedObjectCount}
                        {selectedObjectCount === 1 ? 'object' : 'objects'} will be deleted</strong
                    >
                    <span>
                        {formatStoredSize(inspection.estimatedFreedBytes)} freed ({inspection.estimatedFreedClusters}
                        {inspection.estimatedFreedClusters === 1 ? ' cluster' : ' clusters'})
                    </span>
                {:else if inspection}
                    <strong>Deletion blocked</strong>
                    <span>Resolve the issues above before continuing.</span>
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
