<script lang="ts">
    import type { ProgramGenerationRow } from '../../features/program-generation/workflow.svelte';
    import { programNameError } from '../../features/program-generation/workflow.svelte';
    import type { ProgramGenerationInspection } from '../transport';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        volumeName: string;
        inspection: ProgramGenerationInspection | null;
        rows: ProgramGenerationRow[];
        loading: boolean;
        busy: boolean;
        error: string;
        onselectionchange: (targetObjectId: string, selected: boolean) => void;
        onnamechange: (targetObjectId: string, name: string) => void;
        onselectall: (selected: boolean) => void;
        onconfirm: () => void;
        oncancel: () => void;
    }

    let {
        volumeName,
        inspection,
        rows,
        loading,
        busy,
        error,
        onselectionchange,
        onnamechange,
        onselectall,
        onconfirm,
        oncancel,
    }: Props = $props();

    const selectedCount = $derived(rows.filter((row) => row.selected).length);
    const selectableCount = $derived(Math.min(rows.length, inspection?.availableProgramNumbers.length ?? 0));
    const allSelected = $derived(selectableCount > 0 && selectedCount === selectableCount);
    const someSelected = $derived(selectedCount > 0 && !allSelected);
    const invalidSelectedCount = $derived(
        rows.filter((row) => row.selected && programNameError(row.programName) !== null).length,
    );
    const canConfirm = $derived(
        !loading && !busy && inspection !== null && selectedCount > 0 && invalidSelectedCount === 0,
    );
    const confirmLabel = $derived(
        busy ? 'Generating' : `Generate ${selectedCount} ${selectedCount === 1 ? 'Program' : 'Programs'}`,
    );

    function cancel(): void {
        if (!busy) oncancel();
    }

    function formatProgramNumber(programNumber: number | null): string {
        return programNumber === null ? 'No free slot' : String(programNumber).padStart(3, '0');
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide program-generation-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Generate Programs"
        use:modal={{ onescape: cancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="sparkles" size={16} />
                <h2>Generate Programs</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}>
                <Icon name="close" size={15} />
            </button>
        </header>

        <div class="program-generation-content">
            <div class="program-generation-summary">
                <strong>{volumeName}</strong>
                {#if inspection}
                    <span>{inspection.availableProgramNumbers.length} free Program slots</span>
                {/if}
            </div>

            {#if loading}
                <p class="program-generation-state" role="status">Inspecting Program assignments…</p>
            {:else if rows.length === 0}
                <p class="program-generation-state">No unreferenced Sample Banks or Samples can be generated.</p>
            {:else}
                <div class="program-generation-table" role="table" aria-label="Programs to generate">
                    <div class="program-generation-header" role="row">
                        <span class="selection-cell" role="columnheader">
                            <input
                                type="checkbox"
                                aria-label="Select all targets"
                                checked={allSelected}
                                indeterminate={someSelected}
                                disabled={busy}
                                onchange={(event) => onselectall(event.currentTarget.checked)}
                            />
                        </span>
                        <span role="columnheader">Target</span>
                        <span role="columnheader">Program slot</span>
                        <span role="columnheader">Program name</span>
                        <span role="columnheader">Assignment</span>
                    </div>
                    <div class="program-generation-rows">
                        {#each rows as row (row.targetObjectId)}
                            <div class:excluded={!row.selected} class="program-generation-row" role="row">
                                <div class="selection-cell" role="cell">
                                    <input
                                        type="checkbox"
                                        aria-label={`Generate Program for ${row.targetObjectName}`}
                                        checked={row.selected}
                                        disabled={busy}
                                        onchange={(event) =>
                                            onselectionchange(row.targetObjectId, event.currentTarget.checked)}
                                    />
                                </div>
                                <span class="program-generation-target" role="cell">
                                    <strong>{row.targetObjectName}</strong>
                                    <small>{row.targetObjectType === 'SBAC' ? 'Sample Bank' : 'Sample'}</small>
                                </span>
                                <span class="program-generation-slot" role="cell"
                                    >{formatProgramNumber(row.programNumber)}</span
                                >
                                <span role="cell">
                                    <input
                                        type="text"
                                        value={row.programName}
                                        maxlength="8"
                                        autocomplete="off"
                                        disabled={busy || !row.selected}
                                        aria-label={`Program name for ${row.targetObjectName}`}
                                        aria-invalid={row.selected && programNameError(row.programName) !== null}
                                        oninput={(event) => onnamechange(row.targetObjectId, event.currentTarget.value)}
                                    />
                                    {#if row.selected && programNameError(row.programName)}
                                        <small class="program-generation-name-error"
                                            >{programNameError(row.programName)}</small
                                        >
                                    {/if}
                                </span>
                                <span class="program-generation-assignment" role="cell">=SMP</span>
                            </div>
                        {/each}
                    </div>
                </div>
            {/if}

            {#if inspection && inspection.notices.length > 0}
                <details class="program-generation-notices">
                    <summary
                        >{inspection.notices.length} inspection {inspection.notices.length === 1
                            ? 'notice'
                            : 'notices'}</summary
                    >
                    {#each inspection.notices as notice (`${notice.code}:${notice.message}`)}
                        <p>{notice.message}</p>
                    {/each}
                </details>
            {/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer program-generation-footer">
            <span>{selectedCount} selected</span>
            <div class="program-generation-footer-actions">
                <button class="secondary-button" type="button" disabled={busy} onclick={cancel}>Cancel</button>
                <button class="primary-button" type="button" disabled={!canConfirm} onclick={onconfirm}>
                    {confirmLabel}
                </button>
            </div>
        </footer>
    </div>
</div>

<style>
    .program-generation-dialog {
        width: min(1080px, calc(100vw - 32px));
        max-height: min(860px, calc(100vh - 32px));
    }

    .program-generation-content {
        flex: 1 1 auto;
        min-height: 0;
        display: grid;
        grid-auto-rows: max-content;
        align-content: start;
        gap: 10px;
        padding: 12px 14px;
        overflow: auto;
    }

    .program-generation-summary {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 16px;
    }

    .program-generation-summary strong {
        color: var(--color-text-strong);
    }

    .program-generation-summary span,
    .program-generation-state,
    .program-generation-notices {
        color: var(--color-text-muted);
        font-size: 11px;
    }

    .program-generation-state {
        margin: 24px 0;
        text-align: center;
    }

    .program-generation-table {
        border: 1px solid var(--color-border-strong);
        border-radius: 6px;
        overflow: hidden;
    }

    .program-generation-header,
    .program-generation-row {
        display: grid;
        grid-template-columns: 30px minmax(170px, 1.1fr) 95px minmax(160px, 0.9fr) minmax(90px, 0.45fr);
        gap: 12px;
        padding-inline: 10px;
    }

    .program-generation-header {
        align-items: center;
        padding-block: 7px;
        color: var(--color-text-muted);
        background: var(--color-panel-raised);
        font-size: 12px;
    }

    .program-generation-row {
        align-items: start;
        padding-block: 6px;
    }

    .program-generation-row + .program-generation-row {
        border-top: 1px solid var(--color-border-subtle);
    }

    .program-generation-row.excluded > :not(.selection-cell) {
        opacity: 0.55;
    }

    .selection-cell {
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .selection-cell input {
        margin: 0;
    }

    .program-generation-row .selection-cell {
        padding-top: 2px;
    }

    .program-generation-target {
        min-width: 0;
        display: grid;
        gap: 2px;
    }

    .program-generation-target strong {
        overflow: hidden;
        color: var(--color-text-strong);
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .program-generation-target small,
    .program-generation-name-error {
        color: var(--color-text-muted);
        font-size: 10px;
    }

    .program-generation-slot,
    .program-generation-assignment {
        min-height: 28px;
        display: flex;
        align-items: center;
        font-variant-numeric: tabular-nums;
    }

    .program-generation-row input[type='text'] {
        min-width: 0;
        width: 100%;
    }

    .program-generation-name-error {
        display: block;
        margin-top: 3px;
        color: var(--color-danger);
    }

    .program-generation-notices summary {
        cursor: pointer;
    }

    .program-generation-notices p {
        margin: 5px 0 0 18px;
    }

    .program-generation-footer > span {
        color: var(--color-text-muted);
        font-size: 11px;
    }

    .program-generation-footer-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-left: auto;
    }

    @media (max-width: 760px) {
        .program-generation-header {
            display: none;
        }

        .program-generation-row {
            grid-template-columns: 30px 1fr;
        }

        .program-generation-row > :not(.selection-cell) {
            grid-column: 2;
        }

        .program-generation-row .selection-cell {
            grid-column: 1;
            grid-row: 1;
        }
    }
</style>
