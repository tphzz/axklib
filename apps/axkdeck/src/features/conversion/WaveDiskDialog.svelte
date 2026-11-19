<script lang="ts">
    import { tick } from 'svelte';
    import { modal } from '../../lib/modal';
    import type { FileLocation } from '../../lib/storageLocations';
    import Icon from '../../lib/components/Icon.svelte';
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import DiskImportSettings from '../../lib/components/DiskImportSettings.svelte';
    import ConversionRouteHeader from '../../lib/components/ConversionRouteHeader.svelte';
    import ConversionSourceList from '../../lib/components/ConversionSourceList.svelte';
    import ConversionReport from '../../lib/components/ConversionReport.svelte';
    import { filenameError, type WaveDiskWorkflow, type WaveDiskRequest } from './waveDiskWorkflow.svelte';

    let { workflow, request, browse }: { workflow: WaveDiskWorkflow; request: WaveDiskRequest; browse: () => void } =
        $props();
    let rememberedFile = $state<FileLocation | null>(null);
    let filename = $state('Waves.img');
    const rows = $derived(
        request.rows.map((r) => ({
            id: r.id,
            name: r.name,
            kind: 'Wave Data',
            location: r.location ?? [r.volumeName, r.id].filter(Boolean).join(' · '),
            selectable: true,
            dependencies: [],
        })),
    );
    const locked = $derived(Boolean(workflow.locked || request.phase === 'inspecting'));
    const fileError = $derived(request.delivery.kind === 'download' ? filenameError(request.delivery.filename) : '');
    const error = $derived(request.error || fileError);
    const id = $props.id();
    const report = $derived(request.plan?.report ?? null);
    const status = $derived(
        error ||
            (request.phase === 'inspecting'
                ? 'Reading Wave Data'
                : request.phase === 'preparing'
                  ? 'Preparing conversion'
                  : request.phase === 'executing'
                    ? 'Creating disk image'
                    : !rows.length
                      ? `No Wave Data in this ${request.scopeKind}`
                      : !request.selected.length
                        ? 'Select Waves to convert'
                        : request.delivery.kind === 'server' && !request.delivery.file
                          ? 'Choose an output file'
                          : report?.executable
                            ? 'Ready · 1 disk image to create'
                            : report
                              ? 'Conversion blocked'
                              : 'No current plan'),
    );
    let active = $state<'contents' | 'review'>('contents');
    let shell: HTMLDivElement;
    $effect(() => {
        if (!request.error) return;
        active = 'review';
        void tick().then(() => {
            const pane = shell?.querySelector('.import-pane[data-pane="review"] .import-pane-scroll');
            if (pane) pane.scrollTop = 0;
        });
    });
    function mode(kind: string) {
        if (request.delivery.kind === 'server') rememberedFile = request.delivery.file;
        else filename = request.delivery.filename;
        workflow.change({
            delivery: kind === 'server' ? { kind: 'server', file: rememberedFile } : { kind: 'download', filename },
        });
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide dialog-popovers-visible wave-disk-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Convert"
        bind:this={shell}
        use:modal={{ onescape: () => workflow.close() }}
    >
        <header class="dialog-header">
            <h2>Convert</h2>
            <button
                type="button"
                class="icon-button"
                aria-label="Close"
                disabled={request.phase === 'executing'}
                onclick={() => workflow.close()}><Icon name="close" size={15} /></button
            >
        </header>
        <div class="body">
            <ConversionRouteHeader
                diskIcon
                sourceLabel="A-series image"
                source={request.sourceName}
                profile={request.profile?.id}
                targetLabel={request.profile?.route?.targetLabel}
                disabled={locked}
            />
            <DiskImportSettings>
                <div class="endpoint">
                    <strong>Destination</strong><SelectControl
                        label="Destination"
                        class="delivery-select"
                        value={request.delivery.kind}
                        options={[
                            { value: 'server', label: 'Server file' },
                            { value: 'download', label: 'Download' },
                        ]}
                        disabled={locked}
                        onchange={mode}
                    />
                    <div class="file-field">
                        <label class="file"
                            ><span>File</span>
                            {#if request.delivery.kind === 'server'}<input
                                    class="dialog-field-control"
                                    readonly
                                    value={request.delivery.file?.displayName ?? ''}
                                    placeholder="Choose a new image"
                                />
                            {:else}<input
                                    class="dialog-field-control"
                                    value={request.delivery.filename}
                                    disabled={locked}
                                    aria-invalid={!!fileError}
                                    aria-describedby={fileError ? `${id}-filename-error` : undefined}
                                    oninput={(e) => workflow.renameDownload(e.currentTarget.value)}
                                />{/if}
                        </label>
                        {#if request.delivery.kind === 'server'}<button
                                type="button"
                                class="secondary-button"
                                disabled={locked}
                                onclick={browse}>Browse...</button
                            >{/if}
                    </div>
                    {#if fileError}<p id={`${id}-filename-error`} class="file-error problem" role="status">
                            {fileError}
                        </p>{/if}
                </div>
            </DiskImportSettings>
            <DiskImportSettings
                ><div class="format">
                    <strong>Disk format</strong><span>Yamaha Wave disk · 720 KiB FAT12</span><span
                        >New image · Mono 12-bit Waves only</span
                    >
                </div></DiskImportSettings
            >
            <div class="fidelity">
                <span>Fidelity</span>
                <div
                    class="dialog-segmented-control dialog-segmented-control--compact"
                    role="group"
                    aria-label="Conversion fidelity"
                >
                    {#each ['BEST', 'STRICT'] as f}<button
                            type="button"
                            aria-pressed={request.fidelity === f}
                            disabled={locked}
                            onclick={() => workflow.change({ fidelity: f === 'BEST' ? 'BEST' : 'STRICT' })}
                            >{f === 'BEST' ? 'Best' : 'Strict'}</button
                        >{/each}
                </div>
            </div>
            <ConversionReport
                {report}
                bind:active
                feedbackNotice={request.error ? '1 issue' : ''}
                disabled={locked}
                sourceSummary={`${request.selected.length} selected`}
                sourceCatalog={rows}
                plannedSummary={report?.executable
                    ? `1 disk image · ${report.actionCount} ${report.actionCount === 1 ? 'Wave' : 'Waves'}`
                    : undefined}
            >
                {#snippet sourceSelection()}
                    <ConversionSourceList
                        objects={rows}
                        selected={request.selected}
                        disabled={locked}
                        bulkLabel={`All Waves in ${request.scopeKind}`}
                        locationLabel="Volume / occurrence"
                        onchange={(ids) => workflow.change({ selected: ids ?? rows.map((r) => r.id) })}
                    />
                {/snippet}
                {#snippet sourceFooter()}<div class="tally">
                        {request.selected.length} selected {request.selected.length === 1 ? 'Wave' : 'Waves'}
                    </div>{/snippet}
                {#snippet capacity()}<div class="media"><span>Media</span><span>720 KiB FAT12</span></div>{/snippet}
                {#snippet feedback()}{#if request.error}<p class="problem" role="alert">
                            {request.error}
                        </p>{/if}{/snippet}
            </ConversionReport>
        </div>
        <footer class="dialog-footer">
            <span class="dialog-footer-status" role="status">{status}</span>
            <div class="dialog-footer-actions">
                <button
                    type="button"
                    class="secondary-button"
                    disabled={request.phase === 'executing'}
                    onclick={() => workflow.close()}
                    >{request.phase === 'unconfirmed' || request.phase === 'save-again' ? 'Close' : 'Cancel'}</button
                >
                {#if request.phase === 'idle' && request.error && request.rows.length && request.selected.length}<button
                        type="button"
                        class="secondary-button"
                        onclick={() => void workflow.prepare()}>Prepare again</button
                    >{/if}
                <button
                    type="button"
                    class="primary-button"
                    disabled={!workflow.canCreate && !['unconfirmed', 'save-again'].includes(request.phase)}
                    onclick={() => void workflow.execute()}
                    >{request.phase === 'unconfirmed'
                        ? 'Check status'
                        : request.phase === 'save-again'
                          ? 'Save disk image'
                          : 'Create disk image'}</button
                >
            </div>
        </footer>
    </div>
</div>

<style>
    .dialog-backdrop {
        grid-template: minmax(0, 1fr) / minmax(0, 1fr);
    }
    .wave-disk-dialog {
        width: min(1120px, calc(100% - 32px));
        height: min(614px, calc(100% - 32px));
        max-width: none;
        max-height: none;
        container: disk-import / size;
    }
    .body {
        display: flex;
        flex: 1;
        flex-direction: column;
        min-height: 0;
        padding: 12px;
        gap: 4px;
    }
    .endpoint,
    .format,
    .fidelity,
    .file,
    .file-field,
    .media {
        display: flex;
        align-items: center;
        min-width: 0;
        gap: 8px;
        font-size: var(--dialog-body-font-size);
    }
    .endpoint,
    .format,
    .fidelity {
        width: 100%;
        flex-wrap: wrap;
    }
    .endpoint strong,
    .format strong {
        min-width: 106px;
    }
    .endpoint :global(.delivery-select) {
        width: min(145px, 100%);
    }
    .file-field {
        flex: 1 1 282px;
    }
    .file {
        flex: 1;
    }
    .file input {
        min-width: 0;
        flex: 1;
    }
    .format span,
    .file span,
    .fidelity span,
    .media span:first-child {
        color: var(--color-text-muted);
    }
    .media {
        justify-content: space-between;
        min-height: 25px;
        border-bottom: 1px solid var(--color-border);
    }
    .problem {
        color: var(--color-danger);
        overflow-wrap: anywhere;
    }
    .file-error {
        flex-basis: 100%;
        margin: 0;
    }
    .tally {
        display: flex;
        align-items: center;
        min-height: 28px;
        padding-inline: 6px;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    @container disk-import (max-width:600px) {
        .body {
            overflow-y: auto;
        }
        .body :global(.import-review-layout) {
            flex: 1 0 240px;
        }
        .file-field {
            flex-basis: 100%;
        }
        .endpoint strong,
        .format strong {
            min-width: 0;
        }
    }
</style>
