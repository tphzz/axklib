<script lang="ts">
    import type { MutationCapacityWorkflow } from '../../features/mutation/capacityWorkflow.svelte';
    import { modal } from '../modal';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import Icon from './Icon.svelte';
    let { workflow }: { workflow: MutationCapacityWorkflow } = $props();
    const request = $derived(workflow.request);
</script>

{#if request}
    <div class="dialog-backdrop" role="presentation">
        <div
            class="dialog-shell"
            role="dialog"
            aria-modal="true"
            aria-label="Sampler load capacity"
            use:modal={{ onescape: () => workflow.cancel() }}
        >
            <header class="dialog-header">
                <h2>Sampler load capacity</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Cancel sampler capacity review"
                    disabled={request.review.busy}
                    onclick={() => workflow.cancel()}><Icon name="close" size={14} /></button
                >
            </header>
            <div class="capacity-body">
                <ImportCapacityControl
                    review={request.review}
                    disabled={request.checking}
                    onchange={() => void workflow.inspect()}
                />
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status"
                    >{request.checking
                        ? 'Checking sampler load capacity'
                        : request.review.message || 'Review the sampler load target'}</span
                >
                <div class="dialog-footer-actions">
                    <button
                        class="secondary-button"
                        type="button"
                        disabled={request.review.busy}
                        onclick={() => workflow.cancel()}>Cancel</button
                    >
                    <button
                        class="primary-button"
                        type="button"
                        disabled={request.checking ||
                            request.review.busy ||
                            !request.review.admission ||
                            !request.review.admission.allowed}
                        onclick={() => void workflow.submit()}>Continue</button
                    >
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .dialog-backdrop {
        padding: 12px;
    }
    .dialog-shell {
        width: min(520px, 100%);
        max-height: 100%;
    }
    .capacity-body {
        padding: 12px;
    }
    .dialog-footer {
        flex-wrap: wrap;
    }
    .dialog-footer-status {
        flex-basis: 180px;
    }
</style>
