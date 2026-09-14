<script lang="ts">
    import { tick } from 'svelte';
    import { browserUploadSource } from '../clientUploadSource';
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import { importDestination } from '../../features/import/packageDestinations';
    import type { FloppyImportWorkflow } from '../../features/import/floppyWorkflow.svelte';
    import Icon from './Icon.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import ImportPlanReview from './ImportPlanReview.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';
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
            r?.inspection?.complete &&
            r.selected.length > 0 &&
            !!importDestination(r.mode, r.partitionIndex, r.volumeName),
    );
    const ready = $derived(!controlsDisabled && r?.plan?.valid && !r.dirty);
    const status = $derived(
        workflow.completion.message ||
            r?.error ||
            (r?.status === 'loading'
                ? 'Inspecting floppy images'
                : r?.status === 'planning'
                  ? 'Reviewing import'
                  : !r?.members.length
                    ? 'Choose floppy images'
                    : r.inspection && !r.inspection.complete
                      ? `Add companion disk ${r.inspection.nextRequiredIndex ?? ''}`
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
    const recovery = $derived(
        ['unconfirmed', 'checking', 'refresh-failed', 'refreshing'].includes(workflow.completion.phase),
    );
    const showResults = $derived(
        Boolean(
            r?.error ||
            r?.inspection?.issues.length ||
            workflow.completion.warnings.length ||
            r?.plan?.conflicts.length ||
            r?.plan?.allocation.length ||
            r?.plan?.programSlotPlacements.length ||
            r?.plan?.opaqueSequences.length,
        ),
    );
    let results = $state<HTMLDivElement>();
    async function review() {
        await workflow.review();
        await tick();
        if (results) results.scrollTop = 0;
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
                    <ImportSourceChoice
                        label="Floppy sources"
                        heading="Choose floppy images"
                        description="A-series floppy or companion disk set"
                        workspaceDetail="Select floppy images from a configured workspace"
                        computerDetail="Select local floppy images"
                        computerAvailable={true}
                        onchooseworkspace={() => void workflow.chooseWorkspace()}
                        onchooselocal={() => input?.click()}
                    />
                {:else}
                    <div class="floppy-source-toolbar">
                        <div class="floppy-summary">
                            <strong>{r.inspection?.label || r.members[0]?.name || 'Floppy images'}</strong>
                            <small
                                >{selection.included.size} of {r.inspection?.objects.length ?? 0} objects selected · {r
                                    .members.length}
                                {r.members.length === 1 ? 'disk' : 'disks'} · {formatStoredSize(bytes)}</small
                            >
                        </div>
                        <button
                            class="secondary-button"
                            disabled={controlsDisabled}
                            title="Add floppy images from a storage location"
                            onclick={() => void workflow.chooseWorkspace()}
                            ><Icon name="folder" size={13} />Workspace</button
                        >
                        <button
                            class="secondary-button"
                            disabled={controlsDisabled}
                            title="Add floppy images from this computer"
                            onclick={() => input?.click()}><Icon name="upload" size={13} />Computer</button
                        >
                    </div>
                    <div class="floppy-members" aria-label="Source disks">
                        {#each r.members as member (member.id)}<div>
                                <span title={member.name}>{member.name}</span><button
                                    class="icon-button"
                                    aria-label={`Remove ${member.name}`}
                                    title={`Remove ${member.name}`}
                                    disabled={controlsDisabled}
                                    onclick={() => void workflow.remove(member.id)}
                                    ><Icon name="close" size={12} /></button
                                >
                            </div>{/each}
                    </div>
                    <ImportDestinationChooser
                        mode={r.mode}
                        partitionIndex={r.partitionIndex}
                        volumeName={r.volumeName}
                        partitions={destinations.partitions}
                        volumes={destinations.volumes}
                        disabled={controlsDisabled}
                        onmode={(mode) => workflow.setMode(mode)}
                        onvolume={(index, name) => workflow.setDestination('existing', index, name)}
                        onpartition={(index) =>
                            workflow.setDestination(r.mode, index, r.mode === 'existing' ? '' : r.volumeName)}
                        onname={(name) => workflow.setDestination(r.mode, r.partitionIndex, name)}
                    />
                    <div class="floppy-review">
                        <section class="floppy-object-section" aria-label="Floppy contents">
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
                                    />Name</label
                                ><span>Size</span>
                            </div>
                            <div class="floppy-rows">
                                {#each groups as [type, label]}
                                    {@const objects = r.inspection?.objects.filter((o) => o.objectType === type) ?? []}
                                    {#if objects.length}<h3>{label}</h3>{/if}
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
                                                ><strong>{object.displayName || object.name || 'Unnamed'}</strong
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
                                            ><strong>{file.path}</strong><small
                                                >Configuration or auxiliary file; not imported</small
                                            ></span
                                        ><small>{formatStoredSize(file.sizeBytes)}</small>
                                    </div>{/each}
                            </div>
                        </section>
                        {#if showResults}<div class="floppy-results" aria-label="Import results" bind:this={results}>
                                <ImportPlanReview
                                    plan={r.plan}
                                    busy={r.status === 'planning'}
                                    targetName={r.volumeName || 'new volume'}
                                    renames={r.renames}
                                    programSlots={r.programSlots}
                                    opaqueSequenceActions={r.opaqueSequenceActions}
                                    onrename={(id, name) => workflow.rename(id, name)}
                                    onprogramslot={(id, slot) => workflow.programSlot(id, slot)}
                                    onprogramstart={(id, slot) => workflow.programStart(id, slot)}
                                    onopaquesequenceaction={(id, action) => workflow.opaqueSequenceAction(id, action)}
                                />
                                {#each r.inspection?.issues ?? [] as issue}<p class="package-warning">
                                        {issue.message}
                                    </p>{/each}
                                {#each workflow.completion.warnings as warning}<p class="package-warning">
                                        {warning}
                                    </p>{/each}
                                {#if r.error}<p class="dialog-error" role="alert">{r.error}</p>{/if}
                            </div>{/if}
                    </div>
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
                    {:else if r.members.length && !['warnings', 'completed'].includes(workflow.completion.phase)}<button
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
    .floppy-dialog {
        width: min(820px, calc(100vw - 40px));
        height: min(650px, calc(100dvh - 48px));
    }
    .floppy-content {
        flex: 1;
        min-height: 0;
        display: flex;
        flex-direction: column;
        gap: 8px;
        padding: 10px 12px;
    }
    .floppy-source-toolbar {
        display: flex;
        align-items: center;
        gap: 8px;
        min-width: 0;
    }
    .floppy-source-toolbar strong {
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .floppy-summary {
        display: grid;
        gap: 4px;
        flex: 1;
        min-width: 0;
    }
    .floppy-summary small {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .floppy-members {
        max-height: 66px;
        overflow-y: auto;
        scrollbar-gutter: stable;
    }
    .floppy-members div {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .floppy-members span {
        flex: 1;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .floppy-review {
        flex: 1;
        min-height: 0;
        display: grid;
        grid-template-rows: minmax(100px, 1fr) auto;
        gap: 8px;
    }
    .floppy-object-section {
        min-height: 0;
        display: flex;
        flex-direction: column;
        overflow: hidden;
        border: 1px solid var(--color-border);
        border-radius: 6px;
    }
    .floppy-table-heading {
        display: flex;
        flex: 0 0 auto;
        align-items: center;
        justify-content: space-between;
        padding: 6px calc(8px + var(--overlay-scrollbar-clearance)) 6px 10px;
        overflow-y: auto;
        scrollbar-gutter: stable;
        color: var(--color-text-muted);
        background: var(--color-panel-raised);
        border-bottom: 1px solid var(--color-border);
    }
    .floppy-table-heading label {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .floppy-rows,
    .floppy-results {
        min-height: 0;
        overflow-y: auto;
        scrollbar-gutter: stable;
        padding-right: calc(8px + var(--overlay-scrollbar-clearance));
    }
    .floppy-rows {
        padding-left: 10px;
    }
    .floppy-rows h3 {
        margin: 10px 0 4px;
    }
    .floppy-results {
        max-height: min(220px, 30vh);
        border-top: 1px solid var(--color-border);
        padding-top: 8px;
    }
    .floppy-row {
        display: grid;
        grid-template-columns: 14px minmax(0, 1fr) auto;
        gap: 8px;
        align-items: start;
        padding: 6px 0;
        border-bottom: 1px solid var(--color-border);
    }
    .floppy-row span {
        min-width: 0;
    }
    .floppy-row strong {
        display: block;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .floppy-row small {
        display: block;
    }
    .excluded {
        opacity: 0.65;
    }
    @media (max-width: 640px) {
        .floppy-source-toolbar {
            display: grid;
            grid-template-columns: repeat(2, minmax(0, 1fr));
        }
        .floppy-summary {
            grid-column: 1 / -1;
        }
    }
</style>
