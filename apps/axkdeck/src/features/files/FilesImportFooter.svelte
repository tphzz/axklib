<script lang="ts">
    import type { FilesImportWorkflow } from './importWorkflow.svelte';
    let {
        workflow,
        dismiss,
        blocked = false,
        review = () => workflow.inspect(),
    }: {
        workflow: FilesImportWorkflow;
        dismiss: () => void;
        blocked?: boolean;
        review?: () => Promise<void>;
    } = $props();
    const failed = $derived(['failed', 'unconfirmed', 'refresh-failed', 'write-failed'].includes(workflow.phase));
</script>

<footer class="dialog-footer">
    <span
        class="dialog-footer-status"
        class:dialog-error={failed}
        role={failed ? 'alert' : 'status'}
        title={workflow.message}>{workflow.message}</span
    >
    <div class="dialog-footer-actions">
        <button
            class="secondary-button"
            type="button"
            data-dialog-initial-focus="caret"
            disabled={!workflow.canDismiss}
            onclick={dismiss}>{workflow.phase === 'warnings' ? 'Done' : 'Cancel'}</button
        >
        {#if ['unconfirmed', 'checking', 'refresh-failed', 'refreshing', 'write-failed'].includes(workflow.phase)}
            <button
                class="primary-button"
                type="button"
                disabled={workflow.busy || (workflow.phase === 'unconfirmed' && workflow.jobId === null)}
                onclick={() => void workflow.submit()}
                >{['unconfirmed', 'checking'].includes(workflow.phase) ? 'Check status' : 'Refresh'}</button
            >
        {:else if workflow.phase !== 'warnings'}
            <button
                class="secondary-button"
                type="button"
                disabled={blocked || !workflow.canInspect}
                onclick={() => void review()}>Review</button
            >
            <button
                class="primary-button"
                type="button"
                disabled={blocked || !workflow.canSubmit}
                onclick={() => void workflow.submit()}>Import</button
            >
        {/if}
    </div>
</footer>
