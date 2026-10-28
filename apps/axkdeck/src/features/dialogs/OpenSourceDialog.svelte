<script lang="ts">
    import { onDestroy, onMount, type ComponentProps } from 'svelte';
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import StorageBrowserPanel from '../../lib/components/StorageBrowserPanel.svelte';
    import OpenSourceImagePanel from './OpenSourceImagePanel.svelte';
    import DeviceTargetDetails from './DeviceTargetDetails.svelte';
    import { deviceTargetLabel } from './deviceTargetPresentation';
    import { useDevices } from '../physical-devices/workflow.svelte';
    import { OpenSourceWorkflow } from './openSource.svelte';
    import SourceTabs from './SourceTabs.svelte';
    import DevicePicker from './DevicePicker.svelte';

    let browser: ComponentProps<typeof StorageBrowserPanel> = $props();
    const devices = useDevices();
    const workflow = new OpenSourceWorkflow(
        devices?.service,
        async (location, prepare) => {
            if (!devices || location.kind !== 'device') return false;
            await devices.images.open(
                [location],
                undefined,
                prepare
                    ? async (candidate) => {
                          devices.images.openProgressCancellable = false;
                          devices.images.openProgressLabel = 'Verifying device metadata';
                          devices.images.openProgressCompleted = 0;
                          devices.images.openProgressTotal = undefined;
                          await prepare(candidate, (progress) => {
                              devices.images.openProgressLabel = progress.label;
                              devices.images.openProgressCompleted = progress.completed;
                              devices.images.openProgressTotal = progress.total;
                          });
                      }
                    : undefined,
            );
            if (
                devices.images.location?.kind === 'device' &&
                devices.images.location.reference.sessionId === location.reference.sessionId &&
                devices.images.location.reference.targetId === location.reference.targetId
            )
                return true;
            throw new Error(devices.images.status || 'Device opening was cancelled or failed.');
        },
        async () => {
            await devices?.images.cleanupOpen();
        },
    );
    let fileInput = $state<HTMLInputElement>();
    onMount(() => {
        void workflow.initialize();
    });
    onDestroy(() => {
        void workflow.dispose();
    });

    async function cancel() {
        if (await workflow.cancel()) browser.oncancel();
    }
    async function open() {
        if (await workflow.open()) browser.oncancel();
    }
</script>

<div
    class="dialog-backdrop storage-picker-backdrop"
    role="presentation"
    onclick={(event) => {
        if (event.target === event.currentTarget) void cancel();
    }}
>
    <div
        class="dialog-shell dialog-shell-wide storage-picker open-source-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Open"
        use:modal={{ onescape: workflow.busy ? undefined : () => void cancel() }}
    >
        <header class="dialog-header">
            <h2>Open</h2>
            <button
                class="icon-button"
                type="button"
                aria-label="Close"
                disabled={workflow.busy}
                onclick={() => void cancel()}><Icon name="close" size={15} /></button
            >
        </header>
        <SourceTabs
            id="open"
            label="Open source"
            value={workflow.tab}
            available={workflow.available}
            reason={workflow.unavailableReason}
            disabled={workflow.busy || workflow.connectionBlocksControls}
            onchange={(tab) => workflow.activate(tab)}
        />
        <div
            class="open-source-panel"
            id="open-panel-Image"
            role="tabpanel"
            aria-labelledby="open-tab-Image"
            hidden={workflow.tab !== 'Image'}
            inert={workflow.tab !== 'Image' || workflow.busy}
        >
            <OpenSourceImagePanel {...browser} active={workflow.tab === 'Image'} oncancel={() => void cancel()} />
        </div>
        {#each ['Device', 'SCSI2SDv6', 'SCSI2SD XML'] as tab}
            <div
                class="open-source-panel"
                id={`open-panel-${tab.replaceAll(' ', '-')}`}
                role="tabpanel"
                aria-labelledby={`open-tab-${tab.replaceAll(' ', '-')}`}
                hidden={workflow.tab !== tab}
            >
                <div class="open-source-content">
                    {#if tab === 'SCSI2SD XML'}
                        <div class="open-source-field">
                            <span>Configuration XML</span>
                            <div class="open-source-file">
                                <span title={workflow.xmlName}>{workflow.xmlName || 'No file selected'}</span>
                                <button
                                    class="secondary-button"
                                    type="button"
                                    disabled={workflow.busy || workflow.connectionBlocksControls}
                                    onclick={() => fileInput?.click()}
                                    ><Icon name="folder-open" size={14} /> Browse</button
                                >
                                <input
                                    bind:this={fileInput}
                                    type="file"
                                    accept=".xml"
                                    aria-label="Configuration XML"
                                    hidden
                                    onchange={(event) => {
                                        const file = event.currentTarget.files?.[0];
                                        event.currentTarget.value = '';
                                        if (file) void workflow.loadXml(file);
                                    }}
                                />
                            </div>
                        </div>
                    {/if}
                    <DevicePicker
                        id={`open-device-${tab.replaceAll(' ', '-')}`}
                        candidates={workflow.visibleCandidates}
                        value={tab === 'Device'
                            ? workflow.candidateId
                            : tab === 'SCSI2SDv6'
                              ? workflow.v6CandidateId
                              : workflow.xmlCandidateId}
                        disabled={workflow.busy ||
                            workflow.listing ||
                            workflow.connectionBlocksControls ||
                            (tab === 'SCSI2SD XML' && !workflow.xml)}
                        refreshDisabled={workflow.readingConfiguration}
                        onchange={(value) => {
                            if (tab === 'Device') workflow.candidateId = value;
                            else if (tab === 'SCSI2SDv6') void workflow.selectV6Device(value);
                            else workflow.xmlCandidateId = value;
                        }}
                        onrefresh={() => void workflow.refresh()}
                    />
                    {#if tab === 'SCSI2SDv6'}
                        <label class="open-source-field"
                            >SCSI target
                            <SelectControl
                                label="SCSI target"
                                value={workflow.v6TargetId}
                                onchange={(value) => (workflow.v6TargetId = value)}
                                options={[
                                    { value: '', label: 'Select a configured target' },
                                    ...workflow.v6Targets.map((target) => ({
                                        value: target.id,
                                        label: deviceTargetLabel(target),
                                    })),
                                ]}
                                disabled={workflow.busy ||
                                    workflow.listing ||
                                    workflow.readingConfiguration ||
                                    !workflow.v6Targets.length}
                            />
                        </label>
                        <DeviceTargetDetails
                            target={workflow.v6Targets.find((target) => target.id === workflow.v6TargetId)}
                        />
                        {#each workflow.selectedTarget?.notices ?? [] as notice}<p class="dialog-warning">
                                {notice}
                            </p>{/each}
                    {/if}
                    {#if tab === 'SCSI2SD XML'}
                        <label class="open-source-field"
                            >SCSI target
                            <SelectControl
                                label="SCSI target"
                                value={workflow.xmlTargetId}
                                onchange={(value) => (workflow.xmlTargetId = value)}
                                options={[
                                    { value: '', label: 'Select a configured target' },
                                    ...workflow.xmlTargets.map((target, index) => ({
                                        value: target.id,
                                        key: `${target.id}:${index}`,
                                        disabled: !target.enabled || !target.eligible,
                                        label: `${target.label} / ${(target.sizeBytes / 1e9).toFixed(2)} GB / ${target.sectorSize} B${!target.enabled ? ' / disabled' : !target.eligible ? ' / unavailable' : target.readOnly ? ' / read-only' : ''}`,
                                    })),
                                ]}
                                disabled={!workflow.xml ||
                                    workflow.busy ||
                                    workflow.validating ||
                                    workflow.connectionBlocksControls}
                            />
                        </label>
                        {#each workflow.selectedTarget?.notices ?? [] as notice}<p class="dialog-warning">
                                {notice}
                            </p>{/each}
                    {/if}
                    <div class="open-source-field">
                        <span id={`open-access-${tab.replaceAll(' ', '-')}`}>Access</span>
                        <div
                            class="dialog-segmented-control"
                            role="group"
                            aria-labelledby={`open-access-${tab.replaceAll(' ', '-')}`}
                        >
                            <button
                                type="button"
                                aria-pressed={workflow.access === 'read-only'}
                                disabled={workflow.busy ||
                                    workflow.listing ||
                                    workflow.validating ||
                                    workflow.connectionBlocksControls ||
                                    workflow.readingConfiguration}
                                onclick={() => (workflow.access = 'read-only')}>Read only</button
                            >
                            <button
                                type="button"
                                aria-pressed={workflow.access === 'read-write'}
                                disabled={workflow.busy ||
                                    workflow.listing ||
                                    workflow.validating ||
                                    workflow.connectionBlocksControls ||
                                    workflow.readingConfiguration ||
                                    !!workflow.writeUnavailableReason}
                                aria-describedby={workflow.writeUnavailableReason
                                    ? `open-access-reason-${tab.replaceAll(' ', '-')}`
                                    : undefined}
                                onclick={() => (workflow.access = 'read-write')}>Read &amp; write</button
                            >
                        </div>
                    </div>
                    <div class="open-source-acknowledgement">
                        {#if workflow.access === 'read-write'}
                            <label
                                ><input
                                    class="dialog-checkbox"
                                    type="checkbox"
                                    bind:checked={workflow.acknowledged}
                                    disabled={workflow.busy ||
                                        workflow.listing ||
                                        workflow.validating ||
                                        workflow.connectionBlocksControls ||
                                        workflow.readingConfiguration ||
                                        !workflow.selectedCandidate ||
                                        ((tab === 'SCSI2SD XML' || tab === 'SCSI2SDv6') && !workflow.selectedTarget)}
                                /> No sampler or other SCSI host is accessing this carrier.</label
                            >
                        {:else}
                            <p id={`open-access-reason-${tab.replaceAll(' ', '-')}`}>
                                {workflow.writeUnavailableReason || 'The device will be opened without write access.'}
                            </p>
                        {/if}
                    </div>
                    {#if workflow.selectedCandidate}
                        <dl class="open-source-identity">
                            <dt>Model</dt>
                            <dd>{workflow.selectedCandidate.model}</dd>
                            <dt>Serial</dt>
                            <dd>{workflow.selectedCandidate.serial}</dd>
                            <dt>Capacity</dt>
                            <dd>{workflow.selectedCandidate.sizeBytes} bytes</dd>
                            <dt>Access</dt>
                            <dd>{workflow.access === 'read-write' ? 'Read & write' : 'Read-only'}</dd>
                        </dl>
                    {/if}
                </div>
                <footer class="dialog-footer">
                    <span class="dialog-footer-status" role="status" title={workflow.status}
                        >{workflow.status ||
                            (workflow.access === 'read-write' ? 'Read & write access' : 'Read-only access')}</span
                    >
                    <div class="dialog-footer-actions">
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={workflow.busy}
                            onclick={() => void cancel()}>Cancel</button
                        >
                        <button
                            class="primary-button"
                            type="button"
                            disabled={!workflow.canOpen}
                            onclick={() => void open()}>Open</button
                        >
                    </div>
                </footer>
            </div>
        {/each}
    </div>
</div>

<style>
    .open-source-panel {
        display: flex;
        flex-direction: column;
        flex: 1 1 auto;
        min-height: 0;
    }
    .open-source-panel[hidden] {
        display: none;
    }
    .open-source-content {
        flex: 1 1 auto;
        min-height: 0;
        overflow: auto;
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 12px;
    }
    .open-source-field {
        display: grid;
        grid-template-columns: 110px minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        font-size: var(--dialog-label-font-size);
    }
    .open-source-file {
        display: flex;
        align-items: center;
        min-width: 0;
        gap: 8px;
    }
    .open-source-file > span {
        flex: 1;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
        font-size: var(--dialog-body-font-size);
    }
    .open-source-identity {
        display: grid;
        grid-template-columns: 110px minmax(0, 1fr);
        gap: 8px;
        margin: 0;
    }
    .open-source-acknowledgement {
        display: flex;
        align-items: center;
        min-height: 32px;
        font-size: var(--dialog-body-font-size);
    }
    .open-source-acknowledgement label {
        display: flex;
        align-items: flex-start;
        gap: 6px;
    }
    .open-source-acknowledgement p {
        margin: 0;
    }
    dt {
        font-size: var(--dialog-label-font-size);
    }
    dd {
        margin: 0;
        overflow-wrap: anywhere;
    }
    @media (max-width: 540px) {
        .open-source-field {
            grid-template-columns: 1fr;
            gap: 4px;
        }
    }
</style>
