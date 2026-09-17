<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import FilesystemNameField from './FilesystemNameField.svelte';
    import type { FilesEditWorkflow } from './editWorkflow.svelte';
    let { workflow }: { workflow: FilesEditWorkflow } = $props();
    const review = $derived(workflow.review!);
    const title = $derived(
        review.kind === 'create'
            ? 'New directory'
            : review.kind === 'rename'
              ? `Rename ${review.entries[0].kind}`
              : 'Delete filesystem entries',
    );
    const nameLabel = $derived(review.kind === 'rename' ? 'Name' : 'Directory name');
    const label = $derived(
        ['ready', 'running', 'refreshing'].includes(workflow.phase)
            ? review.kind === 'create'
                ? 'Create'
                : review.kind === 'rename'
                  ? 'Rename'
                  : 'Delete permanently'
            : workflow.phase === 'unconfirmed' && workflow.jobId !== null
              ? 'Check status'
              : 'Refresh',
    );
    const failed = $derived(['failed', 'refresh-failed', 'unconfirmed'].includes(workflow.phase));
    function submit(event: SubmitEvent): void {
        event.preventDefault();
        void workflow.submit();
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell volume-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: () => workflow.close() }}
    >
        <form class="volume-action-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>{title}</h2>
                <button
                    type="button"
                    class="icon-button"
                    aria-label="Close"
                    disabled={!workflow.canClose}
                    onclick={() => workflow.close()}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="volume-action-content">
                {#if review.kind !== 'delete'}
                    <p class="destination" title={review.entries[0].path || review.entries[0].name}>
                        {review.entries[0].path || review.entries[0].name}
                    </p>
                    <div class="name-control">
                        <span>{nameLabel}</span>
                        <FilesystemNameField
                            label={nameLabel}
                            capabilities={review.capabilities}
                            error={workflow.nameError}
                            initialFocus={review.kind === 'rename' ? 'select' : 'caret'}
                            value={workflow.name}
                            onchange={(name) => (workflow.name = name)}
                            disabled={workflow.phase !== 'ready'}
                        />
                    </div>
                    <p class="name-hint">{review.capabilities.nameHint}</p>
                {:else}
                    <p>Permanently delete the selected entries and all contents of selected directories?</p>
                    <ul class="files-deletion-targets">
                        {#each review.entries as entry (entry.id)}<li title={entry.path}>{entry.path}</li>{/each}
                    </ul>
                    <p>This action cannot be undone.</p>
                {/if}
                <p class="dialog-warning">
                    Raw filesystem changes can break sampler relationships. Relationships are not repaired.
                </p>
            </div>
            <footer class="dialog-footer">
                <span
                    class="dialog-footer-status files-action-status"
                    class:dialog-error={failed}
                    role={failed ? 'alert' : 'status'}
                    title={workflow.message}>{workflow.message}</span
                >
                <div class="dialog-footer-actions">
                    <button
                        class="secondary-button"
                        type="button"
                        disabled={(!workflow.busy && !workflow.canClose) ||
                            workflow.phase === 'refreshing' ||
                            (workflow.busy && (workflow.jobId === null || workflow.cancelling))}
                        onclick={() => (workflow.busy ? void workflow.cancel() : workflow.close())}
                        >{workflow.phase === 'ready' || workflow.phase === 'running' ? 'Cancel' : 'Close'}</button
                    >
                    <button
                        class={review.kind === 'delete' && (workflow.phase === 'ready' || workflow.phase === 'running')
                            ? 'danger-button'
                            : 'primary-button'}
                        type="submit"
                        disabled={!workflow.canSubmit}>{label}</button
                    >
                </div>
            </footer>
        </form>
    </div>
</div>

<style>
    .name-control {
        display: grid;
        gap: 4px;
    }
    .name-control > span {
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .destination,
    .files-action-status,
    .files-deletion-targets li {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .files-action-status {
        flex: 1;
        min-width: 0;
        font-size: var(--dialog-metadata-font-size);
    }
    .volume-action-content .name-hint {
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .files-deletion-targets {
        margin: 0;
        padding: 0 var(--overlay-scrollbar-clearance) 0 0;
        list-style: none;
        max-height: 160px;
        overflow: auto;
        scrollbar-gutter: stable;
    }
</style>
