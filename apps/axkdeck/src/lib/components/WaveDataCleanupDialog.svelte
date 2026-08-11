<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import { compareNamedItems } from '../naturalSort';
    import type { WaveDataOrphanCandidate, WaveDataOrphanInspection } from '../transport';
    import Icon from './Icon.svelte';

    interface Props {
        volumeName: string;
        inspection: WaveDataOrphanInspection | null;
        selectedObjectIds: string[];
        loading: boolean;
        busy: boolean;
        error: string;
        onselectionchange: (objectId: string, selected: boolean) => void;
        onselectall: (selected: boolean) => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let {
        volumeName,
        inspection,
        selectedObjectIds,
        loading,
        busy,
        error,
        onselectionchange,
        onselectall,
        oncancel,
        onconfirm,
    }: Props = $props();
    let selectAllCheckbox = $state<HTMLInputElement>();

    const candidates = $derived(
        (inspection?.candidates ?? []).toSorted((left, right) =>
            compareNamedItems(
                { id: left.objectId, name: left.objectName },
                { id: right.objectId, name: right.objectName },
            ),
        ),
    );
    const selectedIds = $derived(new Set(selectedObjectIds));
    const selectedCandidates = $derived(candidates.filter((candidate) => selectedIds.has(candidate.objectId)));
    const allSelected = $derived(candidates.length > 0 && selectedCandidates.length === candidates.length);
    const someSelected = $derived(selectedCandidates.length > 0 && !allSelected);
    const recoverableBytes = $derived(
        selectedCandidates.reduce((total, candidate) => total + candidate.recoverableBytes, 0),
    );
    const recoverableClusters = $derived(
        selectedCandidates.reduce((total, candidate) => total + candidate.recoverableClusters, 0),
    );
    const responseCapped = $derived(
        inspection !== null && inspection.totalCandidateCount > inspection.candidates.length,
    );

    $effect(() => {
        if (selectAllCheckbox) selectAllCheckbox.indeterminate = someSelected;
    });

    function location(candidate: WaveDataOrphanCandidate): string {
        return [candidate.partitionName, candidate.volumeName].filter(Boolean).join(' · ');
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide wave-data-cleanup-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Clean up Wave Data"
        aria-busy={loading || busy}
        use:modal={{ onescape: busy ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <h2>Clean up Wave Data</h2>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="wave-data-cleanup-content">
            <div class="wave-data-cleanup-summary">
                <strong>Unreferenced Wave Data in {volumeName}</strong>
                <p>Only Wave Data that can be verified as unused is shown.</p>
            </div>

            {#if loading && !inspection}
                <p class="dialog-progress" role="status">Inspecting Wave Data…</p>
            {:else if inspection && candidates.length === 0}
                <div class="wave-data-cleanup-empty">
                    <Icon name="broom" size={22} />
                    <p>No unreferenced Wave Data found in {volumeName}.</p>
                </div>
            {:else if inspection}
                <label class="wave-data-cleanup-select-all">
                    <input
                        bind:this={selectAllCheckbox}
                        class="deletion-checkbox"
                        type="checkbox"
                        checked={allSelected}
                        disabled={loading || busy}
                        onchange={(event) => onselectall(event.currentTarget.checked)}
                    />
                    <span>Delete all ({candidates.length})</span>
                </label>
                {#if responseCapped}
                    <p class="wave-data-cleanup-limit">
                        Showing the first {candidates.length.toLocaleString()} of
                        {inspection.totalCandidateCount.toLocaleString()} candidates. Clean up again for the remainder.
                    </p>
                {/if}
                <div class="wave-data-cleanup-list">
                    {#each candidates as candidate (candidate.objectId)}
                        <label class="wave-data-cleanup-row">
                            <input
                                class="deletion-checkbox"
                                type="checkbox"
                                checked={selectedIds.has(candidate.objectId)}
                                disabled={loading || busy}
                                aria-label={`Delete Wave Data ${candidate.objectName}`}
                                onchange={(event) => onselectionchange(candidate.objectId, event.currentTarget.checked)}
                            />
                            <span class="wave-data-cleanup-copy">
                                <strong class="wave-data-cleanup-name">{candidate.objectName}</strong>
                                <small>
                                    {location(candidate)} · {formatStoredSize(candidate.storedSizeBytes)} stored ·
                                    {formatStoredSize(candidate.recoverableBytes)} recoverable
                                </small>
                            </span>
                        </label>
                    {/each}
                </div>
            {/if}

            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer wave-data-cleanup-footer">
            {#if candidates.length > 0}
                <p class="wave-data-cleanup-footer-summary" aria-live="polite">
                    <strong
                        >{selectedCandidates.length}
                        {selectedCandidates.length === 1 ? 'object' : 'objects'} selected</strong
                    >
                    <span>
                        {formatStoredSize(recoverableBytes)} can be recovered ({recoverableClusters}
                        {recoverableClusters === 1 ? ' cluster' : ' clusters'})
                    </span>
                </p>
            {/if}
            <div class="wave-data-cleanup-actions">
                <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>
                    {candidates.length === 0 && inspection ? 'Close' : 'Cancel'}
                </button>
                {#if candidates.length > 0}
                    <button
                        class="danger-button"
                        type="button"
                        disabled={loading || busy || selectedCandidates.length === 0}
                        onclick={onconfirm}
                    >
                        {busy
                            ? 'Deleting…'
                            : `Delete ${selectedCandidates.length} Wave Data ${
                                  selectedCandidates.length === 1 ? 'object' : 'objects'
                              }`}
                    </button>
                {/if}
            </div>
        </footer>
    </div>
</div>

<style>
    .wave-data-cleanup-dialog {
        display: grid;
        grid-template-rows: auto minmax(0, 1fr) auto;
        max-height: min(78vh, 720px);
    }

    .wave-data-cleanup-content {
        min-height: 220px;
        overflow: hidden;
        padding: 14px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }

    .wave-data-cleanup-summary p,
    .wave-data-cleanup-limit {
        margin: 4px 0 0;
        color: var(--color-text-muted);
    }

    .wave-data-cleanup-select-all,
    .wave-data-cleanup-row {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .wave-data-cleanup-select-all {
        min-height: 32px;
        padding: 0 8px;
        border-radius: 4px;
        background: var(--color-panel-raised);
    }

    .wave-data-cleanup-list {
        min-height: 0;
        overflow: auto;
        border: 1px solid var(--color-border);
        border-radius: 4px;
    }

    .wave-data-cleanup-row {
        min-height: 46px;
        padding: 7px 10px;
    }

    .wave-data-cleanup-row + .wave-data-cleanup-row {
        border-top: 1px solid var(--color-border);
    }

    .wave-data-cleanup-copy {
        min-width: 0;
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .wave-data-cleanup-copy strong,
    .wave-data-cleanup-copy small {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .wave-data-cleanup-copy small,
    .wave-data-cleanup-limit {
        font-size: 11px;
    }

    .wave-data-cleanup-empty {
        min-height: 150px;
        display: grid;
        place-content: center;
        justify-items: center;
        gap: 8px;
        color: var(--color-text-muted);
    }

    .wave-data-cleanup-empty p {
        margin: 0;
    }

    .wave-data-cleanup-footer {
        justify-content: space-between;
    }

    .wave-data-cleanup-footer-summary {
        margin: 0;
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .wave-data-cleanup-footer-summary span {
        color: var(--color-text-muted);
        font-size: 11px;
    }

    .wave-data-cleanup-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-left: auto;
    }
</style>
