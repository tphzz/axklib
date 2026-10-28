<script lang="ts">
    import type { CreateWorkflow } from './workflow.svelte';
    import DeviceFormatScope from './DeviceFormatScope.svelte';
    import { modal } from '../../lib/modal';
    let { workflow }: { workflow: CreateWorkflow } = $props();
    const structures = $derived(
        [...new Set(workflow.plan?.inspection.contents.map((entry) => entry.type) ?? [])].slice(0, 8).join(', '),
    );
</script>

{#if workflow.plan}
    <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
        <div
            class="dialog-shell format-confirmation"
            role="dialog"
            aria-modal="true"
            aria-labelledby="format-confirmation-title"
            use:modal={{ onescape: () => workflow.cancelConfirmation() }}
        >
            <header class="dialog-header">
                <h2 id="format-confirmation-title">Erase device and format Yamaha SFS</h2>
            </header>
            <div class="confirmation-content">
                <DeviceFormatScope inspection={workflow.plan.inspection} partitions={workflow.plan.partitionCount} />
                <p class="dialog-warning">
                    Existing structures: {workflow.plan.inspection.contents.length}. {structures ||
                        'No supported signatures detected; existing data may still be present.'}
                </p>
                <label class="ack"
                    ><input class="dialog-checkbox" type="checkbox" bind:checked={workflow.exclusiveUseAcknowledged} /> No
                    sampler or other SCSI host is accessing this USB carrier.</label
                >
                <label class="ack"
                    ><input class="dialog-checkbox" type="checkbox" bind:checked={workflow.dataLossAcknowledged} /> I have
                    verified this device and accept permanent loss of all existing data on the selected target.</label
                >
                <label class="typed"
                    >Type <code>{workflow.plan.inspection.confirmationText}</code>
                    <input
                        class="dialog-field-control"
                        aria-label="Device erase confirmation"
                        bind:value={workflow.confirmationText}
                        autocomplete="off"
                        spellcheck="false"
                        onkeydown={(event) => {
                            if (event.key === 'Enter') event.preventDefault();
                        }}
                    />
                </label>
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status">{workflow.status}</span>
                <div class="dialog-footer-actions">
                    <button
                        type="button"
                        class="secondary-button"
                        data-dialog-initial-focus
                        onclick={() => workflow.cancelConfirmation()}>Cancel</button
                    >
                    <button
                        type="button"
                        class="danger-button"
                        disabled={!workflow.canFormat}
                        onclick={() => void workflow.format()}>Erase And Format</button
                    >
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .format-confirmation {
        width: min(680px, calc(100vw - 24px));
        max-height: calc(100dvh - 24px);
    }
    .confirmation-content {
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 12px;
        overflow: auto;
        min-height: 0;
    }
    .ack {
        display: flex;
        align-items: flex-start;
        gap: 8px;
        font-size: var(--dialog-body-font-size);
    }
    .typed {
        display: grid;
        gap: 6px;
        font-size: var(--dialog-label-font-size);
    }
    code {
        overflow-wrap: anywhere;
        font-size: var(--dialog-body-font-size);
        user-select: none;
    }
    p {
        margin: 0;
        overflow-wrap: anywhere;
    }
</style>
