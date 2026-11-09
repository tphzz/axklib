<script lang="ts">
    import { tick } from 'svelte';
    import { browserUploadSource } from '../clientUploadSource';
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import { importDestination } from '../../features/import/packageDestinations';
    import type { FloppyImportWorkflow } from '../../features/import/floppyWorkflow.svelte';
    import Icon from './Icon.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import ImportPlanReview from './ImportPlanReview.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';
    import ImportSourceHeader from './ImportSourceHeader.svelte';
    import DiskImportSettings from './DiskImportSettings.svelte';
    import ImportReviewLayout from './ImportReviewLayout.svelte';
    import ImportCapacityDetails from './ImportCapacityDetails.svelte';
    let { workflow }: { workflow: FloppyImportWorkflow } = $props();
    let input = $state<HTMLInputElement>();
    const r = $derived(workflow.request);
    const selection = $derived(workflow.selection());
    const destinations = $derived(workflow.destinations());
    const groups = [
        ['PROG', 'Programs'],
        ['SBAC', 'Sample Banks'],
        ['SBNK', 'Samples'],
        ['SMPL', 'Wave Data'],
        ['SEQU', 'Sequences'],
        ['PRF3', 'Other'],
        ['UNKNOWN', 'Unknown'],
    ];
    const locked = $derived(
        !workflow.completion.canDismiss || (r?.status === 'applying' && workflow.completion.phase === 'idle'),
    );
    const controlsDisabled = $derived(workflow.busy || workflow.completion.locked);
    const canReview = $derived(
        !controlsDisabled &&
            r?.inspection?.canImport &&
            (!r.inspection.requiresAcknowledgement || r.acknowledgeSourceIssues) &&
            r.selected.length > 0 &&
            !!importDestination(r.mode, r.partitionIndex, r.volumeName),
    );
    const ready = $derived(
        !controlsDisabled &&
            r?.plan?.valid &&
            !r.dirty &&
            r.plan.capacity.target === workflow.completion.capacity.target &&
            r.plan.capacity.allowed,
    );
    const status = $derived(
        workflow.completion.message ||
            r?.error ||
            (r?.status === 'loading'
                ? 'Inspecting floppy source'
                : r?.status === 'planning'
                  ? 'Reviewing import'
                  : !r?.members.length
                    ? 'Choose floppy source'
                    : r.inspection && !r.inspection.canImport
                      ? r.inspection.nextRequiredIndex
                          ? `No complete objects; add companion disk ${r.inspection.nextRequiredIndex}`
                          : 'No complete objects available'
                      : r?.inspection?.requiresAcknowledgement && !r.acknowledgeSourceIssues
                        ? 'Review and acknowledge source issues'
                        : ready
                          ? 'Ready to import'
                          : 'Review selection and destination'),
    );
    const bytes = $derived(
        r?.inspection?.objects
            .filter((o) => selection.included.has(o.objectKey))
            .reduce((sum, o) => sum + o.sizeBytes, 0) ?? 0,
    );
    const supported = $derived(r?.inspection?.objects.filter((o) => !o.exclusionReason) ?? []);
    const excludedCount = $derived(
        (r?.inspection?.objects.filter((o) => !!o.exclusionReason).length ?? 0) +
            (r?.inspection?.excludedFiles.filter((file) => file.unreadableObject).length ?? 0),
    );
    const recovery = $derived(
        ['unconfirmed', 'checking', 'refresh-failed', 'refreshing'].includes(workflow.completion.phase),
    );
    let active = $state<'contents' | 'review'>('contents');
    let results = $state<HTMLDivElement>();
    $effect(() => {
        void workflow.completion.warnings.join('\n');
        if (['warnings', 'refresh-failed', 'unconfirmed'].includes(workflow.completion.phase) || r?.error) {
            active = 'review';
            void tick().then(() => {
                const scroller = results?.closest('.import-pane-scroll');
                if (scroller) scroller.scrollTop = 0;
            });
        }
    });
    const issueCount = $derived(
        (r?.plan?.conflicts.length ?? 0) +
            (r?.inspection?.issues.length ?? 0) +
            workflow.completion.warnings.length +
            (r?.error ? 1 : 0),
    );
    async function review() {
        await workflow.review();
        active = 'review';
        await tick();
        const scroller = results?.closest('.import-pane-scroll');
        if (scroller) scroller.scrollTop = 0;
    }
</script>

{#if r}
    <div class="dialog-backdrop" role="presentation">
        <div
            class="dialog-shell dialog-shell-wide dialog-popovers-visible floppy-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Import floppy"
            aria-busy={workflow.busy}
            use:modal={{ onescape: locked ? undefined : () => void workflow.close() }}
        >
            <header class="dialog-header">
                <div>
                    <Icon name="archive" size={16} />
                    <h2>Import floppy</h2>
                </div>
                <button class="icon-button" aria-label="Close" disabled={locked} onclick={() => void workflow.close()}
                    ><Icon name="close" size={15} /></button
                >
            </header>
            <div class="floppy-content">
                <input
                    bind:this={input}
                    type="file"
                    accept=".img,.ima"
                    multiple
                    hidden
                    onchange={(event) => {
                        const files = [...(event.currentTarget.files ?? [])].map(browserUploadSource);
                        event.currentTarget.value = '';
                        void workflow.add(files);
                    }}
                />
                {#if !r.members.length}
                    {#if workflow.directSource}
                        <ImportSourceHeader label="No disks added" summary="">
                            {#snippet actions()}<button
                                    class="secondary-button"
                                    disabled={controlsDisabled}
                                    onclick={() => void workflow.chooseWorkspace(true)}
                                    ><Icon name="folder" size={13} />Add disks...</button
                                >{/snippet}
                        </ImportSourceHeader>
                    {:else}<ImportSourceChoice
                            label="Floppy sources"
                            heading="Choose floppy source"
                            description="A-series floppy or companion disk set"
                            workspaceDetail="Select floppy images or unpacked disk folders"
                            computerDetail="Select local floppy images"
                            computerAvailable={true}
                            onchooseworkspace={() => void workflow.chooseWorkspace()}
                            onchooselocal={() => input?.click()}
                        />{/if}
                    {#if r.error}<p class="dialog-error" role="alert">{r.error}</p>{/if}
                {:else}
                    <ImportSourceHeader
                        label={r.inspection?.label || r.members[0]?.name || 'Floppy source'}
                        summary={`${selection.included.size} selected · ${supported.length} available · ${excludedCount} excluded · ${r.inspection?.members.length || r.members.length} ${(r.inspection?.members.length || r.members.length) === 1 ? 'disk' : 'disks'} · ${formatStoredSize(bytes)}`}
                        members={r.members}
                        disabled={controlsDisabled}
                        onremove={(id) => workflow.remove(id)}
                    >
                        {#snippet actions()}
                            <button
                                class="secondary-button"
                                disabled={controlsDisabled}
                                title="Add floppy images or unpacked disk folders from a storage location"
                                onclick={() => void workflow.chooseWorkspace()}
                                ><Icon name="folder" size={13} />{workflow.directSource
                                    ? 'Add disks...'
                                    : 'Workspace'}</button
                            >
                            {#if !workflow.directSource}<button
                                    class="secondary-button"
                                    disabled={controlsDisabled}
                                    title="Add floppy images from this computer"
                                    onclick={() => input?.click()}><Icon name="upload" size={13} />Computer</button
                                >{/if}
                        {/snippet}
                    </ImportSourceHeader>
                    <DiskImportSettings>
                        <ImportDestinationChooser
                            compact
                            mode={r.mode}
                            partitionIndex={r.partitionIndex}
                            volumeName={r.volumeName}
                            partitions={destinations.partitions}
                            volumes={destinations.volumes}
                            disabled={controlsDisabled}
                            onmode={(mode) => workflow.setMode(mode)}
                            onvolume={(index, name) => workflow.setDestination('existing', index, name)}
                            onpartition={(index) => workflow.setPartition(index)}
                            onname={(name) => workflow.setDestination(r.mode, r.partitionIndex, name)}
                        />
                        <ImportCapacityControl
                            compact
                            showSummary={false}
                            review={workflow.completion.capacity}
                            disabled={controlsDisabled}
                            onchange={() => void workflow.review()}
                        />
                    </DiskImportSettings>
                    {#if r.inspection?.requiresAcknowledgement}
                        <label class="floppy-source-acknowledgement">
                            <input
                                class="dialog-checkbox"
                                type="checkbox"
                                checked={r.acknowledgeSourceIssues}
                                disabled={controlsDisabled}
                                onchange={(event) => workflow.acknowledgeSourceIssues(event.currentTarget.checked)}
                            />
                            Import available objects despite source issues
                        </label>
                    {/if}
                    <ImportReviewLayout
                        bind:active
                        contentsSummary={`${selection.included.size} selected / ${supported.length}`}
                        reviewSummary={issueCount
                            ? `${issueCount} ${issueCount === 1 ? 'issue' : 'issues'}`
                            : `Import into ${r.volumeName || 'new volume'}`}
                        reviewNotice={issueCount ? `${issueCount} ${issueCount === 1 ? 'issue' : 'issues'}` : ''}
                    >
                        {#snippet columns()}
                            <div class="floppy-table-heading">
                                <label
                                    ><input
                                        class="dialog-checkbox"
                                        type="checkbox"
                                        aria-label="Select all objects"
                                        disabled={controlsDisabled || !supported.length}
                                        checked={supported.length > 0 && selection.included.size === supported.length}
                                        indeterminate={selection.included.size > 0 &&
                                            selection.included.size < supported.length}
                                        onchange={(event) => workflow.selectAll(event.currentTarget.checked)}
                                    />Object</label
                                ><span>Size</span>
                            </div>
                        {/snippet}
                        {#snippet contents()}
                            <div class="floppy-rows">
                                {#each groups as [type, label]}
                                    {@const objects = r.inspection?.objects.filter((o) => o.objectType === type) ?? []}
                                    {#if objects.length}<h3>{label}<span>{objects.length}</span></h3>{/if}
                                    {#each objects as object (object.objectKey)}
                                        <label
                                            class="floppy-row"
                                            class:excluded={!!object.exclusionReason}
                                            title={object.exclusionReason ||
                                                (selection.required.has(object.objectKey)
                                                    ? 'Required by a selected object'
                                                    : object.displayName)}
                                        >
                                            <input
                                                class="dialog-checkbox"
                                                type="checkbox"
                                                aria-label={`Import ${object.displayName || object.name}`}
                                                checked={selection.included.has(object.objectKey)}
                                                disabled={controlsDisabled ||
                                                    !!object.exclusionReason ||
                                                    selection.required.has(object.objectKey)}
                                                onchange={(event) =>
                                                    workflow.toggle(object.objectKey, event.currentTarget.checked)}
                                            />
                                            <span
                                                ><strong>{object.displayName || object.name || 'Unnamed'}</strong><small
                                                    >{object.sources
                                                        .map((source) => `${source.memberName}: ${source.path}`)
                                                        .join('; ')}</small
                                                >{#if object.exclusionReason}<small>{object.exclusionReason}</small
                                                    >{:else if selection.required.has(object.objectKey)}<small
                                                        >Required</small
                                                    >{/if}</span
                                            ><small>{formatStoredSize(object.sizeBytes)}</small>
                                        </label>
                                    {/each}
                                {/each}
                                {#if r.inspection?.excludedFiles.length}<h3>Excluded files</h3>{/if}
                                {#each r.inspection?.excludedFiles ?? [] as file}<div class="floppy-row excluded">
                                        <Icon name="archive" size={13} /><span
                                            ><strong>{file.path}</strong><small>{file.memberName}</small><small
                                                >{file.reason}</small
                                            ></span
                                        ><small>{formatStoredSize(file.sizeBytes)}</small>
                                    </div>{/each}
                            </div>
                        {/snippet}
                        {#snippet review()}
                            <div class="floppy-results" bind:this={results}>
                                {#each workflow.completion.warnings as warning}<p class="package-warning">
                                        {warning}
                                    </p>{/each}
                                {#if r.error}<p class="dialog-error" role="alert">{r.error}</p>{/if}
                                <ImportPlanReview
                                    partitions={destinations.partitions}
                                    plan={r.plan}
                                    busy={controlsDisabled}
                                    compact
                                    stale={r.dirty}
                                    targetName={r.volumeName || 'new volume'}
                                    renames={r.renames}
                                    programSlots={r.programSlots}
                                    opaqueSequenceActions={r.opaqueSequenceActions}
                                    onrename={(id, name) => workflow.rename(id, name)}
                                    onprogramslot={(id, slot) => workflow.programSlot(id, slot)}
                                    onprogramstart={(id, slot) => workflow.programStart(id, slot)}
                                    onopaquesequenceaction={(id, action) => workflow.opaqueSequenceAction(id, action)}
                                >
                                    {#snippet capacity()}<ImportCapacityDetails
                                            review={workflow.completion.capacity}
                                            admission={r.plan?.capacity}
                                            pending={r.dirty || r.status === 'planning'}
                                        />{/snippet}
                                </ImportPlanReview>
                                {#each r.inspection?.issues ?? [] as issue}<p class="package-warning">
                                        {issue.message}
                                    </p>{/each}
                            </div>
                        {/snippet}
                    </ImportReviewLayout>
                {/if}
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status" aria-live="polite" title={status}>{status}</span>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" disabled={locked} onclick={() => void workflow.close()}
                        >{workflow.completion.phase === 'warnings' || workflow.completion.phase === 'completed'
                            ? 'Done'
                            : workflow.completion.phase === 'refresh-failed'
                              ? 'Close'
                              : 'Cancel'}</button
                    >
                    {#if recovery}<button
                            class="primary-button"
                            disabled={workflow.completion.busy ||
                                (workflow.completion.phase === 'unconfirmed' && !workflow.completion.canCheck)}
                            onclick={() => void workflow.recover()}
                            >{['unconfirmed', 'checking'].includes(workflow.completion.phase)
                                ? 'Check status'
                                : 'Refresh'}</button
                        >
                    {:else if !['warnings', 'completed'].includes(workflow.completion.phase)}<button
                            class="secondary-button"
                            disabled={!canReview}
                            onclick={() => void review()}>Review</button
                        ><button
                            class="primary-button"
                            disabled={!ready}
                            title={ready ? '' : status}
                            onclick={() => void workflow.apply()}>Import</button
                        >{/if}
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .dialog-backdrop {
        grid-template: minmax(0, 1fr) / minmax(0, 1fr);
    }
    .floppy-dialog {
        width: min(1120px, calc(100% - 32px));
        height: min(650px, calc(100% - 32px));
        max-width: none;
        max-height: none;
        container: disk-import / size;
    }
    .floppy-content {
        flex: 1;
        min-height: 0;
        display: flex;
        flex-direction: column;
        gap: 4px;
        padding: 12px;
    }
    .floppy-source-acknowledgement {
        display: flex;
        align-items: center;
        gap: 6px;
        min-height: 28px;
        font-size: var(--dialog-body-font-size);
    }
    .floppy-table-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
        height: 24px;
        padding: 0 6px;
        font-size: var(--dialog-label-font-size);
    }
    .floppy-table-heading label {
        display: flex;
        align-items: center;
        gap: 12px;
    }
    .floppy-rows h3 {
        display: flex;
        align-items: center;
        justify-content: space-between;
        height: 22px;
        margin: 0;
        padding: 0 6px;
        background: var(--color-panel-raised);
        color: var(--color-text);
        font-size: var(--dialog-label-font-size);
        font-weight: 400;
    }
    .floppy-row {
        display: grid;
        grid-template-columns: 14px minmax(0, 1fr) auto;
        gap: 12px;
        align-items: start;
        min-height: 40px;
        padding: 6px;
    }
    .floppy-row span {
        min-width: 0;
    }
    .floppy-row strong {
        display: block;
        font-size: var(--dialog-body-font-size);
        overflow-wrap: anywhere;
    }
    .floppy-row small {
        display: block;
        font-size: var(--dialog-metadata-font-size);
        line-height: 14px;
        color: var(--color-text-muted);
    }
    .excluded strong {
        color: var(--color-text-muted);
    }
    .floppy-results {
        min-width: 0;
    }
    @container disk-import (max-width: 600px) {
        .floppy-content {
            overflow-y: auto;
        }
        .floppy-content :global(.import-review-layout) {
            flex: 1 0 240px;
        }
    }
</style>
