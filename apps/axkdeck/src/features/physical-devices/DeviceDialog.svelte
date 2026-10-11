<script lang="ts">
    import type { DeviceWorkflow } from './workflow.svelte';
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    let { workflow }: { workflow: DeviceWorkflow } = $props();
    const pending = $derived(
        workflow.operations.filter((item) => ['PREPARED', 'RECOVERY_REQUIRED'].includes(item.state)),
    );
    async function loadXml(event: Event) {
        const file = (event.currentTarget as HTMLInputElement).files?.[0];
        if (!file) return;
        if (file.size > 262144) {
            workflow.status = 'Configuration XML exceeds 256 KiB.';
            return;
        }
        workflow.xml = await file.text();
    }
</script>

<div class="dialog-backdrop dialog-backdrop-top" role="presentation">
    <div
        class="dialog-shell device-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="device-dialog-title"
        use:modal={{ onescape: workflow.busy ? undefined : () => void workflow.close() }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="hard-drive" size={16} />
                <h2 id="device-dialog-title">Device recovery</h2>
            </div>
            <button
                class="icon-button"
                type="button"
                title="Close"
                aria-label="Close device dialog"
                disabled={workflow.busy}
                onclick={() => void workflow.close()}><Icon name="close" size={16} /></button
            >
        </header>
        <div class="device-content">
            <label class="field"
                >Device
                <select
                    bind:value={workflow.candidateId}
                    disabled={workflow.busy || !!workflow.connection}
                    data-dialog-initial-focus
                >
                    <option value="">Select a device</option>
                    {#each workflow.visibleCandidates as candidate}
                        <option value={candidate.id}
                            >{candidate.model} / {candidate.serial} / {(candidate.sizeBytes / 1e9).toFixed(2)} GB / {candidate.sectorSize}
                            B</option
                        >
                    {/each}
                </select>
            </label>
            <label class="field"
                >Layout
                <select bind:value={workflow.layout} disabled={workflow.busy || !!workflow.connection}>
                    <option value="RAW">Single sampler disk</option>
                    <option value="SCSI2SD_V6">SCSI2SD V6 card configuration</option>
                    <option value="SCSI2SD_XML">SCSI2SD XML configuration</option>
                </select>
            </label>
            {#if workflow.layout === 'SCSI2SD_XML'}
                <label class="field"
                    >Configuration XML <input
                        type="file"
                        accept=".xml"
                        disabled={workflow.busy || !!workflow.connection}
                        onchange={(event) => void loadXml(event)}
                    /></label
                >
            {/if}
            <div class="connection-actions">
                <button
                    class="secondary-button"
                    type="button"
                    disabled={workflow.busy || !!workflow.connection}
                    onclick={() => void workflow.refresh()}><Icon name="refresh" size={14} /> Refresh</button
                >
                <button
                    class="secondary-button"
                    type="button"
                    disabled={workflow.busy ||
                        !workflow.visibleCandidates.some((candidate) => candidate.id === workflow.candidateId) ||
                        !!workflow.connection ||
                        (workflow.layout === 'SCSI2SD_XML' && !workflow.xml)}
                    onclick={() => void workflow.connect()}
                    ><Icon name="hard-drive" size={14} /> Connect read-only</button
                >
            </div>
            {#if workflow.connection}
                <dl class="identity">
                    <dt>Device</dt>
                    <dd>{workflow.connection.candidate.model}</dd>
                    <dt>Serial</dt>
                    <dd>{workflow.connection.candidate.serial}</dd>
                    <dt>Carrier</dt>
                    <dd>
                        {workflow.connection.candidate.sizeBytes} bytes, {workflow.connection.candidate.sectorSize} B / sector
                    </dd>
                </dl>
                <label class="field"
                    >Target
                    <select bind:value={workflow.targetId} disabled={workflow.busy}>
                        <option value="">Select a target</option>
                        {#each workflow.connection.targets as target}
                            <option value={target.id}
                                >{target.label} / {target.sizeBytes} bytes / {target.sectorSize} B{target.readOnly
                                    ? ' / read-only'
                                    : ''}</option
                            >
                        {/each}
                    </select>
                </label>
                {#each workflow.target?.notices ?? [] as notice}<p class="dialog-warning">{notice}</p>{/each}
                {#if workflow.connection.invalidated}<p class="dialog-error">
                        This connection is invalidated. Close and select the device again.
                    </p>{/if}
            {/if}
            <p>
                Keep the device connected. A backup is recommended. Recovery journals cannot guarantee restoration after
                hardware failure or power loss.
            </p>
            <label class="acknowledgement"
                ><input
                    class="dialog-checkbox"
                    type="checkbox"
                    bind:checked={workflow.acknowledged}
                    disabled={workflow.busy}
                /> No sampler or other SCSI host is accessing this carrier.</label
            >
            {#if workflow.mode === 'recovery'}
                <p>
                    Close the active device image before connecting for recovery. Recovery accepts only the recorded
                    device and original or replacement bytes.
                </p>
                <label class="field"
                    >Interrupted operation
                    <select bind:value={workflow.recoveryId} disabled={workflow.busy}>
                        <option value="">Select an interrupted operation</option>
                        {#each pending as operation}<option value={operation.operationId}
                                >{operation.targetLabel} / {operation.serial} / {operation.state} / {operation.operationId}</option
                            >{/each}
                    </select>
                </label>
                <div class="operation-list" aria-label="Device operation status">
                    {#each workflow.operations as operation}<p>
                            <strong>{operation.state}</strong>
                            {operation.targetLabel} / {operation.serial}<br /><code>{operation.operationId}</code>
                        </p>{/each}
                    {#each workflow.uncertain as record}<p class="dialog-warning">
                            Unresolved request: {record.displayName}<br /><code>{record.operationId}</code><br
                            />{record.jobId
                                ? 'Check status again before writing.'
                                : 'No acknowledgement received. Do not repeat the write; retain this identity for review.'}
                        </p>{/each}
                    {#if !workflow.operations.length && !workflow.uncertain.length}<p>
                            No retained device operations.
                        </p>{/if}
                </div>
            {/if}
        </div>
        <footer class="dialog-footer">
            <span class="dialog-footer-status" role="status" title={workflow.status}>{workflow.status}</span>
            <div class="dialog-footer-actions">
                {#if workflow.mode === 'recovery'}<button
                        class="secondary-button"
                        type="button"
                        disabled={workflow.busy}
                        onclick={() => void workflow.run('Checking status', () => workflow.checkStatus())}
                        ><Icon name="refresh" size={14} /> Check status</button
                    >{/if}
                <button
                    class="secondary-button"
                    type="button"
                    disabled={workflow.busy}
                    onclick={() => void workflow.close()}>Cancel</button
                >
                <button
                    class="danger-button"
                    type="button"
                    disabled={workflow.busy ||
                        !workflow.acknowledged ||
                        !workflow.target ||
                        workflow.target.readOnly ||
                        !workflow.connection?.writesSupported ||
                        workflow.connection.invalidated ||
                        !workflow.recoveryId}
                    onclick={() => void workflow.recover()}>Recover</button
                >
            </div>
        </footer>
    </div>
</div>

<style>
    .device-dialog {
        width: 620px;
        max-width: calc(100vw - 32px);
        max-height: calc(100vh - 40px);
        display: flex;
        flex-direction: column;
    }
    .device-content {
        padding: 12px;
        overflow: auto;
        min-height: 0;
        font-size: var(--dialog-body-font-size);
        display: grid;
        gap: 10px;
    }
    .field {
        display: grid;
        grid-template-columns: 100px minmax(0, 1fr);
        gap: 8px;
        align-items: center;
        font-size: var(--dialog-label-font-size);
    }
    select,
    input {
        min-width: 0;
        max-width: 100%;
        font-size: 11px;
    }
    .identity {
        display: grid;
        grid-template-columns: 100px minmax(0, 1fr);
        gap: 6px 8px;
        margin: 0;
    }
    dt {
        font-size: 10px;
    }
    dd {
        margin: 0;
        overflow-wrap: anywhere;
    }
    .connection-actions {
        display: flex;
        gap: 8px;
        justify-content: flex-end;
    }
    .acknowledgement {
        display: flex;
        align-items: flex-start;
        gap: 8px;
    }
    .operation-list {
        max-height: 180px;
        overflow: auto;
        border-top: 1px solid var(--color-border);
    }
    p {
        margin: 0;
    }
    .operation-list p {
        padding: 6px 0;
    }
    code {
        overflow-wrap: anywhere;
    }
</style>
