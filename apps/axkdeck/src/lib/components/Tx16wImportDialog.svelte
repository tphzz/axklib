<script lang="ts">
    import { tick } from 'svelte';
    import type { Tx16wImportRequest } from '../../features/import/tx16wWorkflow.svelte';
    import { placementError, suggestedVolume, type PlacementOptions } from '../../features/import/tx16wPlacement';
    import type { ClientUploadSource } from '../clientUploadSource';
    import type { Tx16wImportMode, VolumeImportDestination } from '../transport';
    import { requiredConversionIssues, type ConversionFidelity } from '../conversions';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import ConversionReport from './ConversionReport.svelte';
    import ConversionSourceList from './ConversionSourceList.svelte';
    import ConversionImageDestination from './ConversionImageDestination.svelte';
    import ImportSourceHeader from './ImportSourceHeader.svelte';
    import DiskSourcePicker from './DiskSourcePicker.svelte';
    import DiskImportSettings from './DiskImportSettings.svelte';
    import ImportCapacityDetails from './ImportCapacityDetails.svelte';
    import ImportReviewLayout from './ImportReviewLayout.svelte';
    import type { ImportCompletion } from '../../features/import/importCompletion.svelte';

    let {
        request,
        destinations,
        completion,
        onrecover,
        ontarget,
        onbrowse,
        onmode,
        onfidelity,
        onsources,
        onprepare,
        onadd,
        onremove,
        onconfirm,
        oncancel,
    }: {
        request: Tx16wImportRequest;
        destinations: PlacementOptions;
        completion: ImportCompletion;
        onrecover: () => void;
        ontarget: (target: VolumeImportDestination | null, typing?: boolean) => void;
        onbrowse?: () => void;
        onmode: (mode: Tx16wImportMode) => void;
        onfidelity: (fidelity: ConversionFidelity) => void;
        onsources?: (ids: string[] | undefined) => void;
        onprepare: () => void;
        onadd: (files: ClientUploadSource[]) => void;
        onremove: (id: number) => void | Promise<void>;
        onconfirm: () => void;
        oncancel: () => void;
    } = $props();
    const partitionIndex = $derived(request.target?.partitionIndex ?? null);
    const creating = $derived(request.target?.kind === 'CREATE_VOLUME');
    const error = $derived(placementError(request.target, destinations));
    let editedName = $state<{ destination?: string; value: string }>();
    const newName = $derived(
        editedName && editedName.destination === request.destinationName
            ? editedName.value
            : suggestedVolume(request.members[0]?.sourceName ?? request.sourceName ?? ''),
    );
    let active = $state<'contents' | 'review'>('contents');
    let shell: HTMLDivElement;
    $effect(() => {
        void completion.warnings.join('\n');
        if (
            ['warnings', 'refresh-failed', 'unconfirmed'].includes(completion.phase) ||
            request.error ||
            request.discovery.error
        ) {
            active = 'review';
            void tick().then(() => {
                const scroller = shell?.querySelector('.import-pane[data-pane="review"] .import-pane-scroll');
                if (scroller) scroller.scrollTop = 0;
            });
        }
    });
    const busy = $derived(
        completion.locked ||
            ['opening-target', 'uploading', 'discovering', 'inspecting', 'importing'].includes(request.status),
    );
    const report = $derived(request.plan?.report);
    const sourceObjects = $derived(request.sourceObjects ?? report?.sourceObjects ?? []);
    const typhoon = $derived(request.sourceFormat === 'nuedge.typhoon');
    const sourceLabel = $derived(typhoon ? 'TX16W Typhoon disk set' : 'TX16W disk set');
    const title = $derived(
        request.documentSource ? 'Convert' : typhoon ? 'Import Typhoon disk set' : 'Import TX16W disk set',
    );
    const canConfirm = $derived(
        !error &&
            (!!request.documentSource || request.members.length > 0) &&
            !busy &&
            !request.error &&
            !request.discovery.error &&
            request.status === 'ready' &&
            !!report?.executable &&
            !!request.plan &&
            requiredConversionIssues(request.plan).every((id) => request.acknowledgedIssueIds.includes(id)),
    );
    const partitionVolumes = $derived(destinations.volumes.filter((v) => v.partitionIndex === partitionIndex));
    const existingNames = new Map<number, string>();
    let rememberedImage: string | undefined;
    const canDismiss = $derived(
        completion.canDismiss &&
            (request.status !== 'importing' || ['warnings', 'refresh-failed'].includes(completion.phase)),
    );
    function existingVolume(index: number) {
        if (rememberedImage !== request.destinationName) {
            rememberedImage = request.destinationName;
            existingNames.clear();
        }
        const current = request.target;
        if (current?.kind === 'EXISTING_VOLUME' && current.volumeName)
            existingNames.set(current.partitionIndex, current.volumeName);
        const volumes = destinations.volumes.filter((v) => v.partitionIndex === index);
        return volumes.find((v) => v.volumeName === existingNames.get(index)) ?? volumes[0];
    }
    function choosePartition(index: number) {
        const existing = existingVolume(index);
        ontarget(
            creating || !existing
                ? { kind: 'CREATE_VOLUME', partitionIndex: index, volumeName: newName }
                : { kind: 'EXISTING_VOLUME', partitionIndex: index, volumeName: existing.volumeName },
        );
    }
    function chooseMode(mode: 'existing' | 'create') {
        if (partitionIndex === null) return;
        const existing = existingVolume(partitionIndex);
        ontarget({
            kind: mode === 'create' ? 'CREATE_VOLUME' : 'EXISTING_VOLUME',
            partitionIndex,
            volumeName: mode === 'create' ? newName : (existing?.volumeName ?? ''),
        });
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide dialog-popovers-visible tx16w-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        bind:this={shell}
        use:modal={{
            onescape: () => {
                if (canDismiss) oncancel();
            },
        }}
    >
        <header class="dialog-header">
            <h2>{title}</h2>
            <button class="icon-button" aria-label="Close" disabled={!canDismiss} onclick={oncancel}
                ><Icon name="close" size={15} /></button
            >
        </header>
        <div class="disk-import-body">
            {#if request.documentSource}
                <ConversionImageDestination
                    {sourceLabel}
                    source={request.sourceName}
                    profile={request.destinationProfile ?? report?.profileId}
                    targetLabel={request.targetLabel}
                    destination={request.destinationName}
                    disabled={busy}
                    {onbrowse}
                />
            {:else}
                <ImportSourceHeader
                    label={sourceLabel}
                    summary={`${request.members.length} ${request.members.length === 1 ? 'disk' : 'disks'}${report ? ` · ${report.actionCount} planned objects` : ''}`}
                    members={request.members.map((member) => ({
                        id: member.id,
                        name: member.sourceName,
                        autoAdded: member.origin === 'AUTO_ADDED',
                    }))}
                    disabled={busy}
                    {onremove}
                >
                    {#snippet actions()}
                        <DiskSourcePicker
                            disabled={busy}
                            {onadd}
                            onerror={(message) => {
                                request.error = message;
                            }}
                        />
                    {/snippet}
                </ImportSourceHeader>
            {/if}
            {#snippet capacityControl()}
                <ImportCapacityControl
                    compact
                    showSummary={false}
                    review={completion.capacity}
                    disabled={busy}
                    onchange={onprepare}
                />
            {/snippet}
            <DiskImportSettings>
                {#key request.destinationName}
                    <ImportDestinationChooser
                        compact
                        placement
                        mode={creating ? 'create' : 'existing'}
                        {partitionIndex}
                        volumeName={request.target?.volumeName ?? ''}
                        partitions={destinations.partitions}
                        volumes={destinations.volumes}
                        disabled={completion.locked ||
                            request.status === 'importing' ||
                            request.status === 'opening-target' ||
                            (!!request.documentSource && !request.destinationName)}
                        unavailableModes={{
                            create: partitionIndex === null ? 'Choose a partition first' : undefined,
                            existing: partitionVolumes.length ? undefined : 'No existing volumes in this partition',
                        }}
                        nameError={creating ? error : ''}
                        onmode={chooseMode}
                        onname={(value) => {
                            if (partitionIndex === null) return;
                            editedName = { destination: request.destinationName, value };
                            ontarget({ kind: 'CREATE_VOLUME', partitionIndex, volumeName: value }, true);
                        }}
                        onvolume={(index, volumeName) => {
                            if (index !== null)
                                ontarget({ kind: 'EXISTING_VOLUME', partitionIndex: index, volumeName });
                        }}
                        onpartition={choosePartition}
                    />
                {/key}
                {#if !creating || !error}<small class="placement-note"
                        >{!creating
                            ? 'Existing objects are kept.'
                            : partitionVolumes.length
                              ? 'Existing volumes are kept.'
                              : 'No existing volumes in this partition.'}</small
                    >{/if}
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
                <div class="target-model">{@render capacityControl()}</div>
            </div>
            {#snippet capacityDetails()}<ImportCapacityDetails
                    review={completion.capacity}
                    admission={report?.capacity}
                    pending={['uploading', 'discovering', 'inspecting'].includes(request.status)}
                />{/snippet}
            {#snippet capacity()}
                {#if request.documentSource && report}
                    <details class="capacity-disclosure" open={report.capacity?.allowed === false}>
                        <summary
                            >Sampler capacity <span
                                >{report.capacity
                                    ? report.capacity.allowed
                                        ? 'Fits'
                                        : 'Does not fit'
                                    : 'Not checked'}</span
                            ></summary
                        >
                        {@render capacityDetails()}
                    </details>
                {:else}{@render capacityDetails()}{/if}
            {/snippet}
            {#snippet feedback()}
                {#if request.discovery.error}<div class="discovery-recovery">
                        <p class="dialog-error" role="alert">{request.discovery.message} {request.discovery.error}</p>
                        <button class="secondary-button" disabled={busy} onclick={onprepare}>Retry</button>
                    </div>{/if}
                {#if request.error}<p class="dialog-error" role="alert">{request.error}</p>{/if}
                {#each completion.warnings as warning}<p class="package-warning">{warning}</p>{/each}
            {/snippet}
            {#snippet sourceSelection()}<ConversionSourceList
                    objects={sourceObjects}
                    selected={request.sourceObjectIds}
                    onchange={(ids) => onsources?.(ids)}
                    disabled={completion.locked ||
                        !request.target ||
                        !onsources ||
                        ['importing', 'opening-target'].includes(request.status)}
                />{/snippet}
            {#if report || sourceObjects.length}
                <ConversionReport
                    sourceSelection={sourceObjects.length && onsources ? sourceSelection : undefined}
                    sourceSummary={`${request.sourceObjectIds?.length ?? sourceObjects.filter((o) => o.selectable).length} selected`}
                    report={report ?? null}
                    bind:active
                    bind:acknowledged={request.acknowledgedIssueIds}
                    disabled={busy}
                    {capacity}
                    {feedback}
                    feedbackNotice={completion.warnings.length
                        ? `${completion.warnings.length} ${completion.warnings.length === 1 ? 'warning' : 'warnings'}`
                        : request.error || request.discovery.error
                          ? '1 issue'
                          : ''}
                />
            {:else}
                <ImportReviewLayout
                    bind:active
                    reviewNotice={request.error || request.discovery.error ? '1 issue' : ''}
                >
                    {#snippet contents()}<p class="empty-report">
                            {request.status === 'uploading'
                                ? 'Uploading disk images'
                                : request.status === 'discovering'
                                  ? 'Looking for companion disks...'
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
                    (creating && error ? 'Choose a valid volume name' : '') ||
                    (request.status === 'discovering' || request.discovery.error || !report?.executable
                        ? request.discovery.message
                        : '') ||
                    (request.status === 'opening-target'
                        ? 'Opening destination image'
                        : busy
                          ? 'Preparing conversion'
                          : report
                            ? report.executable
                                ? canConfirm
                                    ? request.documentSource
                                        ? `Ready · ${report.actionCount} objects`
                                        : 'Ready to import'
                                    : 'Resolve the mapping decision'
                                : 'Conversion blocked'
                            : request.sourceObjectIds?.length === 0
                              ? 'Nothing selected'
                              : request.target && !request.members.length && !request.documentSource
                                ? 'Add disks to continue'
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
                    {#if !request.plan && request.sourceObjectIds?.length !== 0 && request.status === 'ready' && !request.discovery.error && (request.members.length || request.documentSource)}<button
                            class="secondary-button"
                            disabled={busy}
                            onclick={onprepare}>Prepare again</button
                        >{/if}
                    <button class="primary-button" disabled={!canConfirm} onclick={onconfirm}
                        >{creating
                            ? 'Create volume and import'
                            : request.documentSource
                              ? 'Add to volume'
                              : 'Import'}</button
                    >
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
    .target-model {
        margin-left: auto;
    }
    .placement-note {
        color: var(--color-text-muted);
    }
    @container disk-import (max-width: 800px) {
        .placement-note {
            display: none;
        }
    }
    .capacity-disclosure {
        border-bottom: 1px solid var(--color-border);
        margin-bottom: 6px;
    }
    .capacity-disclosure summary {
        cursor: pointer;
        min-height: 26px;
        line-height: 26px;
        color: var(--color-text-muted);
    }
    .capacity-disclosure summary span {
        float: right;
        color: var(--color-text);
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
    .discovery-recovery {
        display: flex;
        align-items: start;
        gap: 8px;
        padding-block: 6px;
    }
    .discovery-recovery p {
        flex: 1;
        margin: 0;
    }
    .discovery-recovery button {
        flex: none;
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
