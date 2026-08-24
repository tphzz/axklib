<script lang="ts">
    import type { ImageValidationIssue } from '../transport';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        issues: ImageValidationIssue[];
        loading: boolean;
        error: string;
        repairAvailable: boolean;
        repairing: boolean;
        repairLabel: string;
        repairError: string;
        onrepair: () => void;
        onclose: () => void;
    }

    let { issues, loading, error, repairAvailable, repairing, repairLabel, repairError, onrepair, onclose }: Props =
        $props();

    const allocationBlockerCodes = new Set([
        'SFS_ALLOCATION_BITMAP_COPIES_DIFFER',
        'SFS_ALLOCATION_CROSS_LINK',
        'SFS_ALLOCATION_MISMATCH',
        'SFS_EXTENT_BYTE_TOTAL_MISMATCH',
    ]);
    const allocationBlockers = $derived(issues.filter((issue) => allocationBlockerCodes.has(issue.code)));
</script>

<div class="dialog-backdrop dialog-backdrop-top" role="presentation">
    <div
        class="dialog-shell integrity-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="image-integrity-title"
        use:modal
    >
        <header class="dialog-header">
            <div>
                <Icon name="info" size={17} />
                <h2 id="image-integrity-title">Image integrity</h2>
            </div>
            <button
                class="icon-button"
                type="button"
                aria-label="Close image integrity"
                title="Close"
                onclick={onclose}
            >
                <Icon name="close" size={16} />
            </button>
        </header>

        <div class="integrity-content">
            {#if loading}
                <p class="integrity-status">Checking image integrity...</p>
            {:else if error}
                <p class="integrity-error">{error}</p>
            {:else}
                {#if allocationBlockers.length > 0}
                    <section class="allocation-warning" aria-label="Allocation warning">
                        <h3>Alteration is disabled for this image</h3>
                        <p>
                            The stored SFS allocation metadata is inconsistent. Writing could overwrite existing objects
                            or make the image unreadable.
                        </p>
                        <p>Browsing and export remain available.</p>
                        {#if repairAvailable}
                            <p>
                                These malformed extent byte totals can be repaired into a separate image copy. The
                                source image remains unchanged.
                            </p>
                        {:else}
                            <p>Preserve this image and continue from a known-good backup or a newly authored image.</p>
                        {/if}
                    </section>
                {:else}
                    <p class="integrity-status">No blocking SFS allocation issue was found.</p>
                {/if}

                {#if issues.length > 0}
                    <div class="issue-list" aria-label="Validation issues">
                        {#each issues as issue, index (`${issue.code}:${issue.objectId ?? ''}:${index}`)}
                            <article class="issue-row">
                                <div class="issue-heading">
                                    <strong>{issue.message}</strong>
                                    <span class="issue-severity">{issue.severity}</span>
                                </div>
                                <small>{issue.code}</small>
                                {#if issue.samplerPath}
                                    <small>{issue.samplerPath}</small>
                                {/if}
                            </article>
                        {/each}
                    </div>
                {:else}
                    <p class="integrity-status">The image has no reported validation issues.</p>
                {/if}
                {#if repairError}
                    <p class="integrity-error repair-error">{repairError}</p>
                {/if}
                {#if repairing && repairLabel}
                    <p class="integrity-status repair-status">{repairLabel}</p>
                {/if}
            {/if}
        </div>

        <footer class="dialog-footer">
            {#if repairAvailable}
                <button class="primary-button" type="button" data-dialog-initial-focus onclick={onrepair}>
                    {repairing ? 'Cancel repair' : 'Repair copy...'}
                </button>
            {/if}
            <button
                class="secondary-button"
                type="button"
                data-dialog-initial-focus={!repairAvailable}
                disabled={repairing}
                onclick={onclose}>Close</button
            >
        </footer>
    </div>
</div>

<style>
    .integrity-dialog {
        width: min(720px, calc(100vw - 40px));
        max-height: min(760px, calc(100vh - 40px));
    }

    .integrity-content {
        min-height: 140px;
        overflow-y: auto;
        padding: 16px;
    }

    .allocation-warning {
        border: 1px solid var(--color-danger);
        padding: 12px;
    }

    .allocation-warning h3,
    .allocation-warning p,
    .integrity-status,
    .integrity-error {
        margin: 0;
    }

    .allocation-warning h3,
    .integrity-error {
        color: var(--color-danger);
    }

    .allocation-warning p {
        margin-top: 8px;
        line-height: 1.45;
    }

    .repair-error {
        margin-top: 12px;
    }

    .repair-status {
        margin-top: 12px;
    }

    .issue-list {
        margin-top: 14px;
        border: 1px solid var(--color-border);
    }

    .issue-row {
        padding: 10px 12px;
    }

    .issue-row + .issue-row {
        border-top: 1px solid var(--color-border);
    }

    .issue-heading {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 16px;
    }

    .issue-severity,
    .issue-row small {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }

    .issue-row small {
        display: block;
        margin-top: 4px;
    }
</style>
