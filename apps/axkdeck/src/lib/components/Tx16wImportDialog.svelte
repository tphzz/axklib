<script lang="ts">
    import { untrack } from 'svelte';
    import type { Tx16wImportRequest, Tx16wVolumeOption } from '../../features/import/tx16wWorkflow.svelte';
    import { browserUploadSource } from '../clientUploadSource';
    import type { Tx16wImportMode } from '../transport';
    import { requiredConversionIssues, type ConversionFidelity } from '../conversions';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import ConversionReport from './ConversionReport.svelte';
    import type { ImportCompletion } from '../../features/import/importCompletion.svelte';

    let {
        request,
        volumeOptions,
        completion,
        onrecover,
        ontarget,
        onmode,
        onfidelity,
        onprepare,
        onadd,
        onremove,
        onconfirm,
        oncancel,
    }: {
        request: Tx16wImportRequest;
        volumeOptions: Tx16wVolumeOption[];
        completion: ImportCompletion;
        onrecover: () => void;
        ontarget: (target: Tx16wVolumeOption['target'] | null) => void;
        onmode: (mode: Tx16wImportMode) => void;
        onfidelity: (fidelity: ConversionFidelity) => void;
        onprepare: () => void;
        onadd: (files: ReturnType<typeof browserUploadSource>[]) => void;
        onremove: (id: number) => void;
        onconfirm: () => void;
        oncancel: () => void;
    } = $props();
    let partitionIndex = $state<number | null>(untrack(() => request.target?.partitionIndex ?? null));
    let targetSelected = $state(true);
    const busy = $derived(completion.locked || ['uploading', 'inspecting', 'importing'].includes(request.status));
    const report = $derived(request.plan?.report);
    const canConfirm = $derived(
        targetSelected &&
            !busy &&
            request.status === 'ready' &&
            !!report?.executable &&
            !!request.plan &&
            requiredConversionIssues(request.plan).every((id) => request.acknowledgedIssueIds.includes(id)) &&
            volumeOptions.some(
                (option) =>
                    option.target.partitionIndex === request.target?.partitionIndex &&
                    option.target.volumeName === request.target?.volumeName,
            ),
    );
    const partitions = $derived(
        Array.from(
            new Map(
                volumeOptions.map((option) => [
                    option.target.partitionIndex,
                    { partitionIndex: option.target.partitionIndex, name: option.partitionName },
                ]),
            ).values(),
        ),
    );
    const volumes = $derived(
        volumeOptions.map((option) => ({ ...option.target, name: option.partitionName, label: option.label })),
    );
    const canDismiss = $derived(
        completion.canDismiss && (!busy || ['refresh-failed', 'warnings'].includes(completion.phase)),
    );
    function selectTarget(index: number | null, name: string) {
        partitionIndex = index;
        const option = volumeOptions.find(
            (option) => option.target.partitionIndex === index && option.target.volumeName === name,
        );
        targetSelected = !!option;
        ontarget(option?.target ?? null);
    }
    function addFiles(input: HTMLInputElement) {
        const files = Array.from(input.files ?? []).map(browserUploadSource);
        input.value = '';
        if (files.length) onadd(files);
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide tx16w-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import TX16W disk set"
        use:modal={{
            onescape: () => {
                if (canDismiss) oncancel();
            },
        }}
    >
        <header class="dialog-header">
            <h2>Import TX16W disk set</h2>
            <button class="icon-button" aria-label="Close" disabled={!canDismiss} onclick={oncancel}
                ><Icon name="close" size={15} /></button
            >
        </header>
        <div class="conversion-controls">
            {#if request.destinationName}<div class="target-name" title={request.destinationName}>
                    {request.destinationName}
                </div>{/if}
            <ImportCapacityControl
                review={completion.capacity}
                admission={report?.capacity}
                disabled={busy}
                onchange={onprepare}
            />
            <ImportDestinationChooser
                mode="existing"
                {partitionIndex}
                volumeName={request.target?.volumeName ?? ''}
                {partitions}
                {volumes}
                disabled={busy}
                unavailableModes={{ create: 'TX16W conversion requires an existing volume' }}
                onmode={() => undefined}
                onname={() => undefined}
                onvolume={selectTarget}
                onpartition={(index) => selectTarget(index, '')}
            />
            <div class="mode-controls">
                <div class="dialog-segmented-control" role="group" aria-label="Import content">
                    <button
                        disabled={busy}
                        aria-pressed={request.importMode === 'HIERARCHY'}
                        onclick={() => onmode('HIERARCHY')}>Programs and samples</button
                    >
                    <button
                        disabled={busy}
                        aria-pressed={request.importMode === 'WAVE_DATA_ONLY'}
                        onclick={() => onmode('WAVE_DATA_ONLY')}>Wave Data only</button
                    >
                </div>
                <div class="dialog-segmented-control" role="group" aria-label="Conversion fidelity">
                    <button
                        disabled={busy}
                        aria-pressed={request.fidelity === 'BEST'}
                        onclick={() => onfidelity('BEST')}>Best</button
                    >
                    <button
                        disabled={busy}
                        aria-pressed={request.fidelity === 'STRICT'}
                        onclick={() => onfidelity('STRICT')}>Strict</button
                    >
                </div>
            </div>
        </div>
        <div class="conversion-body">
            <section class="disk-set" aria-label="Disk set">
                <header>
                    <strong>Source</strong><span
                        >{request.documentSource ? 'TX16W document' : `${request.members.length} images`}</span
                    >
                </header>
                {#if !request.documentSource}
                    <label class="secondary-button"
                        ><Icon name="file-plus" size={14} />Add disks
                        <input
                            type="file"
                            accept=".img,.ima"
                            multiple
                            disabled={busy}
                            onchange={(event) => addFiles(event.currentTarget)}
                        /></label
                    >
                {/if}
                <div class="members">
                    {#each request.members as member (member.id)}
                        <div>
                            <span>{member.sourceName}</span><button
                                class="icon-button"
                                aria-label={`Remove ${member.sourceName}`}
                                disabled={busy}
                                onclick={() => onremove(member.id)}><Icon name="trash" size={14} /></button
                            >
                        </div>
                    {/each}
                </div>
            </section>
            {#if report}<ConversionReport {report} bind:acknowledged={request.acknowledgedIssueIds} disabled={busy} />
            {:else}<div class="empty-report" aria-live="polite">
                    {request.status === 'uploading'
                        ? 'Uploading disk images'
                        : request.status === 'inspecting'
                          ? 'Preparing conversion'
                          : 'No prepared conversion'}
                </div>{/if}
        </div>
        {#if request.error || completion.warnings.length}<div class="dialog-results dialog-error" role="alert">
                {request.error}
                {#each completion.warnings as warning}<p>{warning}</p>{/each}
            </div>{/if}
        <footer class="dialog-footer">
            <span class="dialog-footer-status" role="status"
                >{completion.message ||
                    request.error ||
                    (busy
                        ? 'Preparing conversion'
                        : report
                          ? report.executable
                              ? canConfirm
                                  ? 'Ready to import'
                                  : 'Review mapping losses'
                              : 'Conversion blocked'
                          : 'Choose a destination')}</span
            >
            <div class="dialog-footer-actions">
                <button class="secondary-button" disabled={!canDismiss} onclick={oncancel}
                    >{completion.phase === 'warnings'
                        ? 'Done'
                        : completion.phase === 'refresh-failed'
                          ? 'Close'
                          : 'Cancel'}</button
                >
                {#if ['unconfirmed', 'checking', 'refresh-failed', 'refreshing'].includes(completion.phase)}
                    <button
                        class="primary-button"
                        disabled={completion.busy || (completion.phase === 'unconfirmed' && !completion.canCheck)}
                        onclick={onrecover}
                        >{['unconfirmed', 'checking'].includes(completion.phase) ? 'Check status' : 'Refresh'}</button
                    >
                {:else if completion.phase !== 'warnings'}
                    {#if !request.plan && request.status === 'ready'}<button
                            class="secondary-button"
                            disabled={busy}
                            onclick={onprepare}>Prepare again</button
                        >{/if}
                    <button class="primary-button" disabled={!canConfirm} onclick={onconfirm}>Import</button>
                {/if}
            </div>
        </footer>
    </div>
</div>

<style>
    .tx16w-import-dialog {
        width: min(1000px, calc(100vw - 32px));
        height: min(720px, calc(100vh - 32px));
    }
    .conversion-controls {
        padding: 0 12px 10px;
        flex-shrink: 0;
        border-bottom: 1px solid var(--color-border);
    }
    .target-name {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        padding-top: 8px;
    }
    .mode-controls {
        display: flex;
        flex-wrap: wrap;
        gap: 12px;
        margin-top: 8px;
        justify-content: space-between;
    }
    .conversion-body {
        display: flex;
        gap: 16px;
        flex: 1;
        min-height: 0;
        padding: 0 12px;
    }
    .disk-set {
        flex: 0 0 180px;
        display: flex;
        flex-direction: column;
        min-height: 0;
    }
    .disk-set header {
        display: flex;
        align-items: center;
        gap: 8px;
        height: 32px;
        flex-shrink: 0;
    }
    .disk-set header span {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .disk-set label {
        position: relative;
        overflow: hidden;
    }
    input[type='file'] {
        position: absolute;
        inset: 0;
        opacity: 0;
        cursor: pointer;
        width: 100%;
    }
    .members {
        flex: 1;
        min-height: 0;
        overflow: auto;
    }
    .members > div {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 5px 0;
    }
    .members span {
        flex: 1;
        min-width: 0;
        overflow-wrap: anywhere;
    }
    .empty-report {
        flex: 1;
        padding: 12px 0;
    }
    .dialog-results {
        max-height: 80px;
        overflow: auto;
        margin: 8px 12px;
    }
    @media (max-width: 600px) {
        .disk-set {
            flex-basis: 115px;
        }
        .conversion-body {
            gap: 8px;
        }
    }
</style>
