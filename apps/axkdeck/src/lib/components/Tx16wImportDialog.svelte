<script lang="ts">
    import { tick, untrack } from 'svelte';
    import type { Tx16wImportRequest, Tx16wVolumeOption } from '../../features/import/tx16wWorkflow.svelte';
    import { browserUploadSource } from '../clientUploadSource';
    import type { Tx16wImportMode } from '../transport';
    import { requiredConversionIssues, type ConversionFidelity } from '../conversions';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import ConversionReport from './ConversionReport.svelte';
    import ImportSourceHeader from './ImportSourceHeader.svelte';
    import DiskImportSettings from './DiskImportSettings.svelte';
    import ImportCapacityDetails from './ImportCapacityDetails.svelte';
    import ImportReviewLayout from './ImportReviewLayout.svelte';
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
    let active = $state<'contents' | 'review'>('contents');
    let shell: HTMLDivElement;
    $effect(() => {
        void completion.warnings.join('\n');
        if (['warnings', 'refresh-failed', 'unconfirmed'].includes(completion.phase) || request.error) {
            active = 'review';
            void tick().then(() => {
                const scroller = shell?.querySelector('.import-pane[data-pane="review"] .import-pane-scroll');
                if (scroller) scroller.scrollTop = 0;
            });
        }
    });
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
        class="dialog-shell dialog-shell-wide dialog-popovers-visible tx16w-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import TX16W disk set"
        bind:this={shell}
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
        <div class="disk-import-body">
            <ImportSourceHeader
                label={request.documentSource ? 'Open TX16W document' : 'TX16W disk set'}
                truncateSummary={!!request.documentSource}
                summary={request.documentSource
                    ? request.destinationName
                        ? `Destination image: ${request.destinationName}`
                        : 'TX16W document'
                    : `${request.members.length} ${request.members.length === 1 ? 'disk' : 'disks'}${report ? ` · ${report.actionCount} planned objects` : ''}`}
                members={request.documentSource
                    ? []
                    : request.members.map((member) => ({ id: member.id, name: member.sourceName }))}
                disabled={busy}
                onremove={request.documentSource ? undefined : onremove}
            >
                {#snippet actions()}
                    {#if !request.documentSource}
                        <label class="secondary-button add-disks" aria-disabled={busy}
                            ><Icon name="file-plus" size={14} />Add disks
                            <input
                                type="file"
                                accept=".img,.ima"
                                multiple
                                disabled={busy}
                                onchange={(event) => addFiles(event.currentTarget)}
                            />
                        </label>
                    {/if}
                {/snippet}
            </ImportSourceHeader>
            <DiskImportSettings>
                <ImportDestinationChooser
                    compact
                    explainUnavailable
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
                <ImportCapacityControl
                    compact
                    showSummary={false}
                    review={completion.capacity}
                    disabled={busy}
                    onchange={onprepare}
                />
            </DiskImportSettings>
            <div class="mode-controls">
                <div class="mode-option">
                    <span class="mode-label">Import mode</span>
                    <div
                        class="dialog-segmented-control dialog-segmented-control--compact conversion-mode"
                        role="group"
                        aria-label="Import content"
                    >
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
                </div>
                <div class="mode-option">
                    <span class="mode-label fidelity-label">Fidelity</span>
                    <div
                        class="dialog-segmented-control dialog-segmented-control--compact"
                        role="group"
                        aria-label="Conversion fidelity"
                    >
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
            {#snippet capacity()}<ImportCapacityDetails
                    review={completion.capacity}
                    admission={report?.capacity}
                    pending={['uploading', 'inspecting'].includes(request.status)}
                />{/snippet}
            {#snippet feedback()}
                {#if request.error}<p class="dialog-error" role="alert">{request.error}</p>{/if}
                {#each completion.warnings as warning}<p class="package-warning">{warning}</p>{/each}
            {/snippet}
            {#if report}
                <ConversionReport
                    {report}
                    bind:active
                    bind:acknowledged={request.acknowledgedIssueIds}
                    disabled={busy}
                    {capacity}
                    {feedback}
                    feedbackNotice={completion.warnings.length
                        ? `${completion.warnings.length} ${completion.warnings.length === 1 ? 'warning' : 'warnings'}`
                        : request.error
                          ? '1 issue'
                          : ''}
                />
            {:else}
                <ImportReviewLayout bind:active reviewNotice={request.error ? '1 issue' : ''}>
                    {#snippet contents()}<p class="empty-report">
                            {request.status === 'uploading'
                                ? 'Uploading disk images'
                                : request.status === 'inspecting'
                                  ? 'Preparing conversion'
                                  : 'No prepared conversion'}
                        </p>{/snippet}
                    {#snippet review()}{@render feedback()}{@render capacity()}{/snippet}
                </ImportReviewLayout>
            {/if}
        </div>
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
    .dialog-backdrop {
        grid-template: minmax(0, 1fr) / minmax(0, 1fr);
    }
    .tx16w-import-dialog {
        width: min(1120px, calc(100% - 32px));
        height: min(650px, calc(100% - 32px));
        max-width: none;
        max-height: none;
        container: disk-import / size;
    }
    .disk-import-body {
        display: flex;
        flex: 1;
        flex-direction: column;
        gap: 4px;
        min-height: 0;
        padding: 12px;
    }
    .mode-controls {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 4px;
        flex: none;
        min-height: 28px;
    }
    .mode-label {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
        width: 70px;
    }
    .mode-option {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 4px;
        min-width: 0;
        max-width: 100%;
    }
    .fidelity-label {
        width: auto;
        margin: 0 8px;
    }
    .conversion-mode {
        width: 282px;
        max-width: 100%;
        grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .conversion-mode button {
        width: auto;
    }
    .add-disks {
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
    .add-disks:focus-within {
        outline: 1px solid var(--state-focus);
        outline-offset: 2px;
    }
    .empty-report {
        color: var(--color-text-muted);
        margin: 6px 0;
    }
    @container disk-import (max-width: 420px) {
        .mode-label {
            width: 100%;
        }
        .fidelity-label {
            width: auto;
            margin: 0;
        }
    }
    @container disk-import (max-width: 600px) {
        .disk-import-body {
            overflow-y: auto;
        }
        .disk-import-body :global(.import-review-layout) {
            flex: 1 0 240px;
        }
    }
</style>
