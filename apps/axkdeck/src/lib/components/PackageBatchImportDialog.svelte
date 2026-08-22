<script lang="ts">
    import { tick } from 'svelte';
    import type { BatchPackageItem, PackageBatchDestinationStrategy } from '../../features/import/packageBatchTypes';
    import type {
        ImportDestinationMode,
        ImportPartitionOption,
        ImportVolumeOption,
    } from '../../features/import/packageDestinations';
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { ImageSessionPackageImportPlan, PackageOpaqueSequenceDecision } from '../transport';
    import Icon from './Icon.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';
    import PackageBatchConflictControls from './PackageBatchConflictControls.svelte';
    import PackageBatchDestinationChooser from './PackageBatchDestinationChooser.svelte';
    import PackageBatchItemsTable from './PackageBatchItemsTable.svelte';

    const renameConflictCodes = new Set([
        'SFS_NAME_CONFLICT',
        'SFS_TARGET_NAME_AMBIGUOUS',
        'FAT12_NAME_CONFLICT',
        'FAT12_TARGET_NAME_AMBIGUOUS',
        'ISO9660_NAME_CONFLICT',
        'ISO9660_TARGET_NAME_AMBIGUOUS',
    ]);

    interface Props {
        desktop: boolean;
        canChangeSources: boolean;
        items: BatchPackageItem[];
        plan: ImageSessionPackageImportPlan | null;
        destinationStrategy: PackageBatchDestinationStrategy;
        destinationMode: ImportDestinationMode;
        destinationPartitionIndex: number | null;
        destinationVolumeName: string;
        partitionOptions: ImportPartitionOption[];
        volumeOptions: ImportVolumeOption[];
        separateVolumesAvailable: boolean;
        volumeNames: Record<string, string>;
        renames: Record<string, string>;
        programSlots: Record<string, number>;
        opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
        hasUnvalidatedChanges: boolean;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        completedFiles: number;
        totalFiles: number;
        progress: number;
        error: string;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
        ondestinationstrategy: (strategy: PackageBatchDestinationStrategy) => void;
        ondestinationmode: (mode: ImportDestinationMode) => void;
        ondestinationvolume: (partitionIndex: number | null, volumeName: string) => void;
        ondestinationpartition: (partitionIndex: number) => void;
        ondestinationname: (name: string) => void;
        onrenamevolume: (itemId: string, name: string) => void;
        onrename: (itemId: string, nodeId: string, name: string) => void;
        onprogramslot: (itemId: string, nodeId: string, slot: number) => void;
        onprogramstart: (placementId: string, start: number) => void;
        ontoggleselected: (itemId: string, selected: boolean) => void;
        ontoggleall: (selected: boolean) => void;
        onopaquesequenceaction: (
            itemId: string,
            nodeId: string,
            action: PackageOpaqueSequenceDecision['action'],
        ) => void;
        onreplan: () => Promise<void>;
        oncancel: () => void;
        onconfirm: () => void;
    }

    let {
        desktop,
        canChangeSources,
        items,
        plan,
        destinationStrategy,
        destinationMode,
        destinationPartitionIndex,
        destinationVolumeName,
        partitionOptions,
        volumeOptions,
        separateVolumesAvailable,
        volumeNames,
        renames,
        programSlots,
        opaqueSequenceActions,
        hasUnvalidatedChanges,
        status,
        completedFiles,
        totalFiles,
        progress,
        error,
        onchooseworkspace,
        onchooselocal,
        ondestinationstrategy,
        ondestinationmode,
        ondestinationvolume,
        ondestinationpartition,
        ondestinationname,
        onrenamevolume,
        onrename,
        onprogramslot,
        onprogramstart,
        ontoggleselected,
        ontoggleall,
        onopaquesequenceaction,
        onreplan,
        oncancel,
        onconfirm,
    }: Props = $props();
    let batchPackageContent = $state<HTMLDivElement>();
    let batchResults = $state<HTMLElement>();

    const busy = $derived(status === 'loading' || status === 'planning' || status === 'applying');
    const locked = $derived(status === 'applying');
    const changeSources = $derived(items.some((item) => item.localPath !== null) ? onchooselocal : onchooseworkspace);
    const selectedItems = $derived(items.filter((item) => item.selected));
    const selectedCount = $derived(selectedItems.length);
    const canImport = $derived(
        status === 'ready' && Boolean(plan?.valid) && !hasUnvalidatedChanges && selectedCount > 0 && !error,
    );
    const summaries = $derived(hasUnvalidatedChanges ? [] : (plan?.packages ?? []));
    const totalObjects = $derived(
        hasUnvalidatedChanges
            ? selectedItems.reduce((total, item) => total + item.inspection.objects.length, 0)
            : summaries.reduce((total, item) => total + item.objectCount, 0),
    );
    const totalPayload = $derived(
        hasUnvalidatedChanges
            ? selectedItems.reduce((total, item) => total + item.inspection.totalPayloadBytes, 0)
            : summaries.reduce((total, item) => total + item.payloadBytes, 0),
    );
    const placementNodes = $derived(
        new Set(
            plan?.programSlotPlacements.flatMap((placement) =>
                placement.mappings.map((mapping) => `${mapping.packageIndex}:${mapping.nodeId}`),
            ) ?? [],
        ),
    );
    const actionableRenameNodes = $derived(
        new Set(
            plan?.actions
                .filter((action) => action.actions.includes('CONFLICT'))
                .map((action) => `${action.packageIndex}:${action.nodeId}`) ?? [],
        ),
    );
    const visibleConflicts = $derived(
        hasUnvalidatedChanges
            ? []
            : (plan?.conflicts.filter(
                  (conflict) =>
                      conflict.code !== 'OPAQUE_SEQUENCE_DECISION_REQUIRED' &&
                      conflict.code !== 'SFS_RECORD_CAPACITY_EXHAUSTED' &&
                      !(
                          renameConflictCodes.has(conflict.code) &&
                          (placementNodes.has(`${conflict.packageIndex}:${conflict.nodeId}`) ||
                              actionableRenameNodes.has(`${conflict.packageIndex}:${conflict.nodeId}`))
                      ),
              ) ?? []),
    );
    const showResults = $derived(
        Boolean(error) ||
            (!hasUnvalidatedChanges &&
                Boolean(plan) &&
                (visibleConflicts.length > 0 ||
                    (plan?.opaqueSequences.length ?? 0) > 0 ||
                    (plan?.programSlotPlacements.length ?? 0) > 0)),
    );
    const footerStatus = $derived.by(() => {
        if (status === 'loading') return 'Preparing packages…';
        if (status === 'planning') return 'Planning batch import…';
        if (status === 'applying') return 'Importing packages…';
        if (error) return 'Import failed';
        if (hasUnvalidatedChanges) {
            return selectedCount === 0 ? 'Select at least one package' : 'Changes need checking';
        }
        if (visibleConflicts.length > 0) {
            return `${visibleConflicts.length} issue${visibleConflicts.length === 1 ? '' : 's'} prevent import`;
        }
        if (plan && !plan.valid) return 'Review import issues';
        if (plan?.valid) return 'Ready to import';
        return 'Choose packages to import';
    });

    function opaqueKey(itemId: string, nodeId: string): string {
        return `${itemId}:${nodeId}`;
    }

    async function replanAndRevealSummary(): Promise<void> {
        try {
            await onreplan();
        } catch {
            return;
        }
        await tick();
        if (error) return;
        const tableRows = batchPackageContent?.querySelector<HTMLElement>('.batch-table-rows');
        if (tableRows) tableRows.scrollTop = 0;
        if (batchResults) batchResults.scrollTop = 0;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide batch-package-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="batch-package-title"
        use:modal={{ onescape: locked ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2 id="batch-package-title">Import packages</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={locked} onclick={oncancel}>×</button>
        </header>

        <div class="batch-package-content" bind:this={batchPackageContent}>
            {#if items.length === 0 && status === 'choosing'}
                <ImportSourceChoice
                    label="Package sources"
                    heading="Choose packages"
                    description="Import portable axklib packages or A3K archives into the open SFS image."
                    workspaceDetail="Select one or more package files"
                    computerDetail="Select local package files and upload them"
                    computerAvailable={desktop}
                    {onchooseworkspace}
                    {onchooselocal}
                />
            {:else if status === 'loading'}
                <section class="batch-progress" aria-live="polite">
                    <strong>Preparing packages</strong>
                    <progress value={Math.max(completedFiles, progress)} max={Math.max(1, totalFiles)}></progress>
                    <small>{completedFiles} of {totalFiles} files inspected</small>
                </section>
            {:else}
                <section class="batch-summary" aria-label="Batch summary">
                    <div>
                        <strong
                            >{selectedCount} of {items.length} package{items.length === 1 ? '' : 's'} selected</strong
                        >
                        <small>
                            {destinationStrategy === 'separate'
                                ? `${selectedCount} volume${selectedCount === 1 ? '' : 's'} will be created`
                                : `${selectedCount} package${selectedCount === 1 ? '' : 's'} will be imported into one volume`}
                            · {totalObjects} objects · {formatStoredSize(totalPayload)}
                        </small>
                    </div>
                    {#if canChangeSources}<button
                            class="secondary-button"
                            type="button"
                            disabled={busy}
                            onclick={changeSources}>Change files</button
                        >{/if}
                </section>

                <PackageBatchDestinationChooser
                    strategy={destinationStrategy}
                    mode={destinationMode}
                    partitionIndex={destinationPartitionIndex}
                    volumeName={destinationVolumeName}
                    partitions={partitionOptions}
                    volumes={volumeOptions}
                    separateAvailable={separateVolumesAvailable}
                    disabled={busy}
                    onstrategy={ondestinationstrategy}
                    onmode={ondestinationmode}
                    onvolume={ondestinationvolume}
                    onpartition={ondestinationpartition}
                    onname={ondestinationname}
                />

                <div class="batch-workspace">
                    <div class="batch-table-region">
                        <PackageBatchItemsTable
                            {items}
                            {plan}
                            {destinationStrategy}
                            {destinationVolumeName}
                            {volumeNames}
                            {hasUnvalidatedChanges}
                            {busy}
                            {onrenamevolume}
                            {ontoggleselected}
                            {ontoggleall}
                        />
                    </div>

                    {#if showResults}
                        <section class="batch-results" aria-label="Import results" bind:this={batchResults}>
                            {#if !hasUnvalidatedChanges && plan?.opaqueSequences.length}
                                <section class="batch-sequence-decisions" aria-label="Sequence decisions">
                                    {#each plan.opaqueSequences as sequence (`${sequence.packageIndex}:${sequence.nodeId}`)}
                                        {@const packageIndex = sequence.packageIndex ?? 0}
                                        {@const item = selectedItems[packageIndex]}
                                        {@const selected = item
                                            ? (opaqueSequenceActions[opaqueKey(item.id, sequence.nodeId)] ??
                                              sequence.action)
                                            : sequence.action}
                                        {#if item}<fieldset>
                                                <legend
                                                    >Sequence “{sequence.name || 'Unnamed'}” could not be decoded</legend
                                                >
                                                <small
                                                    >Choose whether to preserve its original bytes or leave it out.</small
                                                >
                                                <label>
                                                    <input
                                                        type="radio"
                                                        name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                                        checked={selected === 'PRESERVE_UNCHANGED'}
                                                        disabled={busy}
                                                        onchange={() =>
                                                            onopaquesequenceaction(
                                                                item.id,
                                                                sequence.nodeId,
                                                                'PRESERVE_UNCHANGED',
                                                            )}
                                                    />
                                                    Preserve unchanged
                                                </label>
                                                <label>
                                                    <input
                                                        type="radio"
                                                        name={`batch-sequence-${packageIndex}-${sequence.nodeId}`}
                                                        checked={selected === 'SKIP'}
                                                        disabled={busy}
                                                        onchange={() =>
                                                            onopaquesequenceaction(item.id, sequence.nodeId, 'SKIP')}
                                                    />
                                                    Skip Sequence
                                                </label>
                                            </fieldset>{/if}
                                    {/each}
                                </section>
                            {/if}

                            {#if !hasUnvalidatedChanges && plan}
                                <PackageBatchConflictControls
                                    items={selectedItems}
                                    {plan}
                                    {renames}
                                    {programSlots}
                                    disabled={busy}
                                    {onrename}
                                    {onprogramslot}
                                    {onprogramstart}
                                />
                            {/if}

                            {#if visibleConflicts.length > 0}
                                <section class="batch-conflicts" role="alert">
                                    <strong
                                        >{visibleConflicts.length} issue{visibleConflicts.length === 1 ? '' : 's'} prevent
                                        import</strong
                                    >
                                    {#each visibleConflicts as conflict (`${conflict.packageIndex}:${conflict.code}:${conflict.nodeId}`)}
                                        <p>{conflict.message}</p>
                                    {/each}
                                </section>
                            {/if}
                            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
                        </section>
                    {/if}
                </div>
            {/if}
        </div>

        <footer class="dialog-footer batch-package-footer">
            {#if items.length > 0}
                <button
                    class="secondary-button"
                    type="button"
                    disabled={busy || selectedCount === 0}
                    onclick={() => void replanAndRevealSummary()}
                >
                    Check conflicts
                </button>
            {/if}
            <p class:error-status={Boolean(error)} class="batch-footer-status" role="status" aria-live="polite">
                {footerStatus}
            </p>
            <div class="batch-package-footer-actions">
                <button class="secondary-button" type="button" disabled={locked} onclick={oncancel}>Cancel</button>
                {#if items.length > 0}
                    <button class="primary-button" type="button" disabled={!canImport} onclick={onconfirm}>
                        {status === 'applying'
                            ? 'Importing…'
                            : `Import ${selectedCount} package${selectedCount === 1 ? '' : 's'}`}
                    </button>
                {/if}
            </div>
        </footer>
    </div>
</div>

<style>
    .batch-package-dialog {
        width: min(1080px, calc(100vw - 32px));
        max-height: min(860px, calc(100vh - 32px));
    }

    .batch-package-content {
        display: flex;
        flex: 1;
        flex-direction: column;
        min-height: 0;
        gap: 8px;
        overflow: hidden;
        padding: 10px 12px;
    }

    .batch-progress,
    .batch-summary {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .batch-progress {
        min-height: 180px;
        flex-direction: column;
        justify-content: center;
    }

    .batch-progress progress {
        width: min(420px, 100%);
    }

    .batch-summary {
        justify-content: space-between;
    }

    .batch-summary div {
        display: grid;
        gap: 4px;
    }

    .batch-summary small,
    .batch-sequence-decisions small {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }

    .batch-workspace,
    .batch-table-region {
        display: grid;
        min-height: 0;
    }

    .batch-workspace {
        flex: 1;
        grid-template-rows: minmax(140px, 1fr) auto;
        gap: 8px;
    }

    .batch-table-region {
        overflow: hidden;
    }

    .batch-results {
        max-height: min(220px, 30vh);
        overflow-y: auto;
        border-top: 1px solid var(--color-border);
        padding-top: 8px;
    }

    .batch-sequence-decisions,
    .batch-conflicts {
        display: grid;
        gap: 10px;
        padding: 10px;
        border: 1px solid #765d2d;
        border-radius: 6px;
        background: rgb(150 108 26 / 10%);
    }

    .batch-sequence-decisions fieldset {
        display: grid;
        gap: 8px;
        border: 0;
        padding: 0;
    }

    .batch-sequence-decisions label {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .batch-conflicts p {
        margin: 0;
    }

    .batch-package-footer {
        flex-wrap: wrap;
        justify-content: flex-start;
    }

    .batch-footer-status {
        margin: 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }

    .batch-footer-status.error-status {
        color: var(--color-danger);
    }

    .batch-package-footer-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-left: auto;
    }
</style>
