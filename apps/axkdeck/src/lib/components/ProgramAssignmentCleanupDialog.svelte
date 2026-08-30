<script lang="ts">
    import type { ProgramAssignmentCleanupRow } from '../../features/program-assignment-cleanup/workflow.svelte';
    import type { ProgramAssignmentCleanupInspection } from '../transport';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        volumeName: string;
        inspection: ProgramAssignmentCleanupInspection | null;
        rows: ProgramAssignmentCleanupRow[];
        loading: boolean;
        busy: boolean;
        error: string;
        onselectall: (selected: boolean) => void;
        onprogramselectionchange: (programObjectId: string, selected: boolean) => void;
        onselectionchange: (programObjectId: string, assignmentOrdinal: number, selected: boolean) => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let {
        volumeName,
        inspection,
        rows,
        loading,
        busy,
        error,
        onselectall,
        onprogramselectionchange,
        onselectionchange,
        oncancel,
        onconfirm,
    }: Props = $props();

    const selectedCount = $derived(rows.filter((row) => row.selected).length);
    const allSelected = $derived(rows.length > 0 && selectedCount === rows.length);
    const someSelected = $derived(selectedCount > 0 && !allSelected);
    const groups = $derived(groupRows(rows));

    function reason(row: ProgramAssignmentCleanupRow): string {
        if (row.reason === 'NONLOCAL_TARGET') return 'Target exists outside this volume';
        if (row.reason === 'AMBIGUOUS_TARGET') return `${row.candidateTargetCount} matching targets`;
        return 'Missing target';
    }

    function cancel(): void {
        if (!busy) oncancel();
    }

    function groupRows(rows: ProgramAssignmentCleanupRow[]) {
        const groups = new Map<string, ProgramAssignmentCleanupRow[]>();
        for (const row of rows) groups.set(row.programObjectId, [...(groups.get(row.programObjectId) ?? []), row]);
        return [...groups.entries()].map(([programObjectId, grouped]) => ({
            programObjectId,
            label: `${String(grouped[0]?.programNumber ?? 0).padStart(3, '0')}: ${grouped[0]?.programName ?? ''}`,
            selectedCount: grouped.filter((row) => row.selected).length,
            rows: grouped,
        }));
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide program-assignment-cleanup-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Clean unresolved Program assignments"
        aria-busy={loading || busy}
        use:modal={{ onescape: cancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="broom" size={16} />
                <h2>Clean unresolved Program assignments</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}>
                <Icon name="close" size={15} />
            </button>
        </header>

        <div class="program-assignment-cleanup-content">
            <div class="program-assignment-cleanup-summary">
                <strong>{volumeName}</strong>
                <span>Remove stored assignments whose Sample or Sample Bank cannot be resolved.</span>
            </div>
            {#if loading && !inspection}
                <p class="program-assignment-cleanup-state" role="status">Inspecting Program assignments…</p>
            {:else if inspection && rows.length === 0}
                <div class="program-assignment-cleanup-state empty">
                    <Icon name="broom" size={22} />No unresolved assignments found.
                </div>
            {:else if inspection}
                <label class="program-assignment-cleanup-select-all">
                    <input
                        class="dialog-checkbox"
                        type="checkbox"
                        aria-label="Select all unresolved assignments"
                        checked={allSelected}
                        indeterminate={someSelected}
                        disabled={busy}
                        onchange={(event) => onselectall(event.currentTarget.checked)}
                    />
                    <span>Select all ({rows.length})</span>
                </label>
                <div class="program-assignment-cleanup-list">
                    {#each groups as group (group.programObjectId)}
                        <section class="program-assignment-cleanup-group">
                            <label class="program-assignment-cleanup-program">
                                <input
                                    class="dialog-checkbox"
                                    type="checkbox"
                                    aria-label={`Select assignments for ${group.label}`}
                                    checked={group.selectedCount === group.rows.length}
                                    indeterminate={group.selectedCount > 0 && group.selectedCount < group.rows.length}
                                    disabled={busy}
                                    onchange={(event) =>
                                        onprogramselectionchange(group.programObjectId, event.currentTarget.checked)}
                                />
                                <strong>{group.label}</strong>
                                <span>{group.rows.length} unresolved</span>
                            </label>
                            {#each group.rows as row (row.assignmentOrdinal)}
                                <label class:excluded={!row.selected} class="program-assignment-cleanup-row">
                                    <input
                                        class="dialog-checkbox"
                                        type="checkbox"
                                        aria-label={`Select Assignment ${row.assignmentOrdinal + 1} ${row.assignmentName}`}
                                        checked={row.selected}
                                        disabled={busy}
                                        onchange={(event) =>
                                            onselectionchange(
                                                row.programObjectId,
                                                row.assignmentOrdinal,
                                                event.currentTarget.checked,
                                            )}
                                    />
                                    <span class="assignment-number">{row.assignmentOrdinal + 1}</span>
                                    <span class="assignment-copy">
                                        <strong>{row.assignmentName}</strong>
                                        <small>
                                            {row.targetObjectType === 'SBAC' ? 'Sample Bank' : 'Sample'} ·
                                            {row.receiveChannelDisplay || 'Unknown receive'} ·
                                            <span>{reason(row)}</span>
                                        </small>
                                    </span>
                                </label>
                            {/each}
                        </section>
                    {/each}
                </div>
            {/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <span class="program-assignment-cleanup-count" aria-live="polite">{selectedCount} selected</span>
            <div class="program-assignment-cleanup-actions">
                <button class="secondary-button" type="button" disabled={busy} onclick={cancel}
                    >{inspection && rows.length === 0 ? 'Close' : 'Cancel'}</button
                >
                {#if rows.length > 0}
                    <button
                        class="danger-button"
                        type="button"
                        disabled={loading || busy || selectedCount === 0}
                        onclick={onconfirm}
                    >
                        {busy
                            ? 'Cleaning…'
                            : `Clean ${selectedCount} ${selectedCount === 1 ? 'assignment' : 'assignments'}`}
                    </button>
                {/if}
            </div>
        </footer>
    </div>
</div>

<style>
    .program-assignment-cleanup-dialog {
        display: grid;
        grid-template-rows: auto minmax(0, 1fr) auto;
        max-height: min(78vh, 720px);
    }
    .program-assignment-cleanup-content {
        min-height: 240px;
        overflow: hidden;
        padding: 14px;
        display: flex;
        flex-direction: column;
        gap: 10px;
    }
    .program-assignment-cleanup-summary {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }
    .program-assignment-cleanup-summary span,
    .program-assignment-cleanup-program span,
    .assignment-copy small,
    .program-assignment-cleanup-count {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .program-assignment-cleanup-select-all,
    .program-assignment-cleanup-program,
    .program-assignment-cleanup-row {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .program-assignment-cleanup-select-all {
        min-height: 28px;
        padding: 0 8px;
        background: var(--color-panel-raised);
        border-radius: 4px;
    }
    .program-assignment-cleanup-list {
        min-height: 0;
        padding-right: 14px;
        overflow: auto;
        scrollbar-gutter: stable;
        border: 1px solid var(--color-border);
        border-radius: 4px;
    }
    .program-assignment-cleanup-group + .program-assignment-cleanup-group {
        border-top: 1px solid var(--color-border-strong);
    }
    .program-assignment-cleanup-program {
        position: sticky;
        top: 0;
        z-index: 1;
        min-height: 30px;
        padding: 0 8px;
        background: var(--color-panel-raised);
    }
    .program-assignment-cleanup-program span {
        margin-left: auto;
    }
    .program-assignment-cleanup-row {
        min-height: 36px;
        padding: 4px 8px 4px 28px;
        border-top: 1px solid var(--color-border);
    }
    .program-assignment-cleanup-row.excluded {
        opacity: 0.55;
    }
    .assignment-number {
        width: 20px;
        color: var(--color-text-muted);
        font-variant-numeric: tabular-nums;
    }
    .assignment-copy {
        min-width: 0;
        display: flex;
        flex-direction: column;
        gap: 1px;
    }
    .assignment-copy strong,
    .assignment-copy small {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .program-assignment-cleanup-state {
        flex: 1;
        display: grid;
        place-content: center;
        color: var(--color-text-muted);
    }
    .program-assignment-cleanup-state.empty {
        justify-items: center;
        gap: 8px;
    }
    .program-assignment-cleanup-count {
        margin-right: auto;
    }
    .program-assignment-cleanup-actions {
        display: flex;
        align-items: center;
        gap: 8px;
    }
</style>
