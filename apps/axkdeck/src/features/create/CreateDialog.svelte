<script lang="ts">
    import type { CreateWorkflow } from './workflow.svelte';
    import type { ImageTransport } from '../../lib/transport';
    import type { DirectoryLocation, FileLocation } from '../../lib/storageLocations';
    import SourceTabs from '../dialogs/SourceTabs.svelte';
    import DevicePicker from '../dialogs/DevicePicker.svelte';
    import DeviceTargetDetails from '../dialogs/DeviceTargetDetails.svelte';
    import { deviceTargetLabel } from '../dialogs/deviceTargetPresentation';
    import CreateImagePanel from '../../lib/components/CreateImagePanel.svelte';
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import SfsPartitionControls from '../../lib/components/SfsPartitionControls.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import DeviceFormatScope from './DeviceFormatScope.svelte';
    import DeviceFormatConfirmation from './DeviceFormatConfirmation.svelte';
    import { modal } from '../../lib/modal';
    let {
        workflow,
        transport,
        directory,
        onbrowse,
        onsuccess,
        onrecovery,
    }: {
        workflow: CreateWorkflow;
        transport: ImageTransport;
        directory: DirectoryLocation | null;
        onbrowse: () => void;
        onsuccess: (file: FileLocation) => void;
        onrecovery: () => void;
    } = $props();
    const selection = $derived(workflow.selection);
    let xmlInput = $state<HTMLInputElement>();
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell create-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Create"
        use:modal={{ onescape: () => void workflow.cancel() }}
    >
        <header class="dialog-header">
            <h2>Create</h2>
            <button
                class="icon-button"
                type="button"
                title="Close"
                aria-label="Close Create"
                disabled={workflow.locked || (workflow.busy && (!workflow.imageBusy || !workflow.imageCancellable))}
                onclick={() => void workflow.cancel()}><Icon name="close" size={15} /></button
            >
        </header>
        <SourceTabs
            id="create"
            label="Create destination"
            value={selection.tab}
            available={selection.available}
            reason={selection.reason}
            disabled={workflow.busy || workflow.locked || workflow.phase === 'complete'}
            onchange={(tab) => workflow.activate(tab)}
        />
        <div
            id="create-panel-Image"
            role="tabpanel"
            aria-labelledby="create-tab-Image"
            class="image-panel"
            hidden={selection.tab !== 'Image'}
        >
            <CreateImagePanel
                {transport}
                {directory}
                {onbrowse}
                oncancel={() => {
                    workflow.imageBusy = false;
                    void workflow.cancel();
                }}
                onsuccess={(file) => {
                    workflow.imageBusy = false;
                    workflow.visible = false;
                    onsuccess(file);
                }}
                onstatechange={({ busy, cancellable, cancel }) => {
                    workflow.imageBusy = busy;
                    workflow.imageCancellable = cancellable;
                    workflow.cancelImage = cancel;
                }}
            />
        </div>
        {#if selection.tab !== 'Image'}
            <div
                id={`create-panel-${selection.tab.replaceAll(' ', '-')}`}
                role="tabpanel"
                aria-labelledby={`create-tab-${selection.tab.replaceAll(' ', '-')}`}
                class="device-panel"
            >
                <div class="device-content">
                    <fieldset disabled={workflow.busy || workflow.locked || workflow.phase === 'complete'}>
                        {#if selection.tab === 'SCSI2SD XML'}
                            <div class="xml-field">
                                <span>{selection.xmlName || 'No configuration XML selected'}</span>
                                <button type="button" class="secondary-button" onclick={() => xmlInput?.click()}
                                    ><Icon name="folder-open" size={14} /> Browse XML</button
                                >
                                <input
                                    bind:this={xmlInput}
                                    type="file"
                                    accept=".xml"
                                    hidden
                                    aria-label="Configuration XML"
                                    onchange={(event) => {
                                        const file = event.currentTarget.files?.[0];
                                        event.currentTarget.value = '';
                                        if (file) void selection.loadXml(file);
                                    }}
                                />
                            </div>
                        {/if}
                        <DevicePicker
                            id="create-device"
                            candidates={selection.visibleCandidates}
                            value={selection.candidateId}
                            disabled={workflow.busy ||
                                workflow.locked ||
                                workflow.phase === 'complete' ||
                                (selection.tab === 'SCSI2SD XML' && !selection.xml)}
                            onchange={(id) => void workflow.select(id)}
                            onrefresh={() => void selection.refresh()}
                        />
                        {#if selection.tab !== 'Device'}<label class="target-field"
                                >SCSI target
                                <SelectControl
                                    label="SCSI target"
                                    value={selection.targetId}
                                    disabled={workflow.busy ||
                                        workflow.locked ||
                                        workflow.phase === 'complete' ||
                                        !selection.connection}
                                    options={[
                                        { value: '', label: 'Select a configured target' },
                                        ...(selection.connection?.targets ?? []).map((target) => ({
                                            value: target.id,
                                            label: deviceTargetLabel(target),
                                        })),
                                    ]}
                                    onchange={(id) => void workflow.selectTarget(id)}
                                />
                            </label>{/if}
                    </fieldset>
                    {#if selection.target}<DeviceTargetDetails target={selection.target} />{/if}
                    {#if workflow.inspection}
                        <DeviceFormatScope inspection={workflow.inspection} partitions={workflow.partitionCount} />
                        <section class="contents" aria-label="Existing device contents">
                            <h3>Existing contents</h3>
                            <div class="contents-scroll">
                                <table>
                                    <thead
                                        ><tr><th>Structure / name</th><th>Range (bytes)</th><th>Validation</th></tr
                                        ></thead
                                    >
                                    <tbody
                                        >{#each workflow.inspection.contents as entry, index (index)}<tr
                                                ><td>{entry.type}{entry.label ? ` / ${entry.label}` : ''}</td><td
                                                    >{entry.offsetBytes.toLocaleString()} to {(
                                                        entry.offsetBytes + entry.sizeBytes
                                                    ).toLocaleString()}</td
                                                ><td
                                                    >{entry.validation === 'STRUCTURE_VALIDATED'
                                                        ? 'Validated structure'
                                                        : 'Signature only'}</td
                                                ></tr
                                            >{/each}</tbody
                                    >
                                </table>
                            </div>
                            {#if !workflow.inspection.contents.length}<p>
                                    No supported signatures detected. Existing data may still be present.
                                </p>{/if}
                        </section>
                        {#if workflow.inspection.mountedVolumes.length}<div class="notices">
                                {#each workflow.inspection.mountedVolumes as volume}<p class="dialog-warning">
                                        Mounted: {volume.device} / {volume.mountPoint} / {volume.filesystem ||
                                            'Filesystem not reported'}
                                        {volume.label}
                                    </p>{/each}
                            </div>{/if}
                        <div class="notices">
                            {#each workflow.inspection.blockedReasons as reason}<p class="dialog-error">
                                    {reason}
                                </p>{/each}{#each workflow.inspection.warnings as warning}<p class="dialog-warning">
                                    {warning}
                                </p>{/each}
                        </div>
                        <SfsPartitionControls
                            options={workflow.inspection.partitionOptions}
                            value={workflow.partitionCount}
                            disabled={workflow.busy || workflow.locked || workflow.phase === 'complete'}
                            onchange={(count) => workflow.setPartitions(count)}
                        />
                    {/if}
                    {#if workflow.operationId}<p class="operation">
                            Operation: <code>{workflow.operationId}</code>
                        </p>{/if}
                    {#if selection.status}<p class="dialog-error" role="alert">{selection.status}</p>{/if}
                </div>
                <footer class="dialog-footer">
                    <span class="dialog-footer-status" role="status"
                        >{workflow.status || (workflow.busy ? 'Reading device configuration...' : '')}</span
                    >
                    <div class="dialog-footer-actions">
                        <button
                            type="button"
                            class="secondary-button"
                            disabled={(workflow.busy && workflow.phase !== 'running') ||
                                (workflow.locked && workflow.phase !== 'running')}
                            onclick={() => void workflow.cancel()}
                            >{workflow.phase === 'complete' ? 'Done' : 'Cancel'}</button
                        >
                        {#if workflow.locked && !workflow.busy}<button
                                type="button"
                                class="primary-button"
                                onclick={() => void workflow.checkStatus()}
                                ><Icon name="refresh" size={14} /> Check status</button
                            >{/if}
                        {#if workflow.phase === 'recovery'}<button
                                type="button"
                                class="secondary-button"
                                onclick={onrecovery}>Device operation status</button
                            >{/if}
                        {#if workflow.phase === 'complete'}<button
                                type="button"
                                class="primary-button"
                                onclick={() => void workflow.openFormatted()}>Open</button
                            >
                        {:else if !workflow.locked}<button
                                type="button"
                                class="secondary-button"
                                disabled={workflow.busy || !selection.target}
                                onclick={() => void workflow.inspect()}
                                ><Icon name="refresh" size={14} /> Inspect again</button
                            >
                            <button
                                type="button"
                                class="primary-button"
                                disabled={!workflow.canReview}
                                onclick={() => void workflow.review()}>Review</button
                            >{/if}
                    </div>
                </footer>
            </div>
        {/if}
    </div>
</div>
{#if workflow.phase === 'confirming'}<DeviceFormatConfirmation {workflow} />{/if}

<style>
    .create-dialog {
        width: min(760px, calc(100vw - 24px));
        height: min(820px, calc(100dvh - 24px));
        max-height: calc(100dvh - 24px);
    }
    .image-panel,
    .device-panel {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }
    .image-panel[hidden] {
        display: none;
    }
    .device-content {
        flex: 1;
        display: flex;
        flex-direction: column;
        min-height: 0;
        gap: 10px;
        overflow: auto;
        padding: 12px;
    }
    fieldset {
        min-width: 0;
        margin: 0;
        padding: 0;
        border: 0;
        display: grid;
        gap: 10px;
    }
    .target-field {
        display: grid;
        grid-template-columns: 110px minmax(0, 1fr);
        gap: 8px;
        align-items: center;
        font-size: var(--dialog-label-font-size);
    }
    .xml-field {
        display: flex;
        gap: 8px;
        align-items: center;
        font-size: var(--dialog-body-font-size);
    }
    .xml-field span {
        min-width: 0;
        flex: 1;
        overflow-wrap: anywhere;
    }
    .contents h3 {
        font-size: var(--dialog-body-font-size);
        margin: 0 0 4px;
    }
    .contents-scroll {
        max-height: 160px;
        overflow: auto;
    }
    table {
        width: 100%;
        border-collapse: collapse;
        table-layout: fixed;
        font-size: var(--dialog-metadata-font-size);
    }
    th {
        position: sticky;
        top: 0;
        background: var(--color-bg-panel);
        text-align: left;
        font-weight: 500;
    }
    th,
    td {
        padding: 5px;
        border-bottom: 1px solid var(--color-border);
        vertical-align: top;
        overflow-wrap: anywhere;
    }
    th:first-child {
        width: 42%;
    }
    th:nth-child(2) {
        width: 36%;
    }
    .notices {
        max-height: 100px;
        overflow: auto;
        display: grid;
        gap: 5px;
    }
    p {
        margin: 0;
        font-size: var(--dialog-body-font-size);
        overflow-wrap: anywhere;
    }
    .operation {
        font-size: var(--dialog-metadata-font-size);
    }
    @media (max-width: 520px) {
        .target-field {
            grid-template-columns: minmax(0, 1fr);
        }
    }
</style>
