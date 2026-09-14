<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import {
        importDestination,
        type ImportDestinationMode,
        type ImportPartitionOption,
        type ImportVolumeOption,
    } from '../../features/import/packageDestinations';
    import type { ImageSessionPackageImportPlan, PackageInspection, PackageOpaqueSequenceDecision } from '../transport';
    import Icon from './Icon.svelte';
    import ImportPlanReview from './ImportPlanReview.svelte';
    import ImportSourceChoice from './ImportSourceChoice.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';
    import type { ImportCompletion } from '../../features/import/importCompletion.svelte';

    interface Props {
        completion: ImportCompletion;
        onrecover: () => void;
        targetName: string;
        destinationMode: ImportDestinationMode;
        destinationPartitionIndex: number | null;
        destinationVolumeName: string;
        partitionOptions: ImportPartitionOption[];
        volumeOptions: ImportVolumeOption[];
        desktop: boolean;
        canChangeSource: boolean;
        sourceName: string;
        inspection: PackageInspection | null;
        plan: ImageSessionPackageImportPlan | null;
        renames: Record<string, string>;
        programSlots: Record<string, number>;
        opaqueSequenceActions?: Record<string, PackageOpaqueSequenceDecision['action']>;
        hasUnvalidatedChanges?: boolean;
        status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
        progress: number;
        error: string;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
        onchange: () => void;
        ondestinationmode: (mode: ImportDestinationMode) => void;
        ondestinationvolume: (partitionIndex: number | null, volumeName: string) => void;
        ondestinationpartition: (partitionIndex: number) => void;
        ondestinationname: (name: string) => void;
        onrename: (nodeId: string, name: string) => void;
        onprogramslot: (nodeId: string, slot: number) => void;
        onprogramstart: (placementId: string, start: number) => void;
        onopaquesequenceaction: (nodeId: string, action: PackageOpaqueSequenceDecision['action']) => void;
        onreplan: () => void;
        oncancel: () => void;
        onconfirm: () => void;
    }

    interface PackageTreeRow {
        key: string;
        depth: number;
        name: string;
        type: string;
    }

    let {
        completion,
        onrecover,
        targetName,
        destinationMode,
        destinationPartitionIndex,
        destinationVolumeName,
        partitionOptions,
        volumeOptions,
        desktop,
        canChangeSource,
        sourceName,
        inspection,
        plan,
        renames,
        programSlots,
        opaqueSequenceActions = {},
        hasUnvalidatedChanges = false,
        status,
        progress,
        error,
        onchooseworkspace,
        onchooselocal,
        onchange,
        ondestinationmode,
        ondestinationvolume,
        ondestinationpartition,
        ondestinationname,
        onrename,
        onprogramslot,
        onprogramstart,
        onopaquesequenceaction,
        onreplan,
        oncancel,
        onconfirm,
    }: Props = $props();

    const busy = $derived(status === 'loading' || status === 'planning' || status === 'applying');
    const locked = $derived(completion.phase !== 'refresh-failed' && (completion.locked || status === 'applying'));
    const canImport = $derived(status === 'ready' && Boolean(plan?.valid) && !hasUnvalidatedChanges);
    const importDisabledReason = $derived(
        canImport
            ? ''
            : status !== 'ready'
              ? 'Wait for package planning to finish.'
              : hasUnvalidatedChanges
                ? 'Review changes before importing.'
                : 'Resolve import issues before importing.',
    );
    const treeRows = $derived(packageTree(inspection));
    const destinationReady = $derived(
        importDestination(destinationMode, destinationPartitionIndex, destinationVolumeName) !== null,
    );
    const canCheckConflicts = $derived(!busy && destinationReady && !!inspection);
    const footerStatus = $derived(
        completion.message ||
            error ||
            (status === 'loading'
                ? 'Inspecting package'
                : status === 'planning'
                  ? 'Reviewing import'
                  : status === 'applying'
                    ? 'Importing'
                    : !sourceName
                      ? 'Choose a package'
                      : !destinationReady
                        ? 'Choose a valid destination'
                        : hasUnvalidatedChanges || !plan
                          ? 'Review changes before importing'
                          : canImport
                            ? 'Ready to import'
                            : 'Resolve import issues'),
    );
    function objectTypeLabel(type: string): string {
        if (type === 'PROG') return 'Program';
        if (type === 'SBAC') return 'Sample Bank';
        if (type === 'SBNK') return 'Sample';
        if (type === 'SMPL') return 'Wave Data';
        if (type === 'SEQU') return 'Sequence';
        if (type === 'VOLUME') return 'Volume';
        return type;
    }

    function packageTree(value: PackageInspection | null): PackageTreeRow[] {
        if (!value) return [];
        const objects = new Map(value.objects.map((object) => [object.nodeId, object]));
        const children = new Map<string, string[]>();
        for (const relationship of value.relationships) {
            const targets = children.get(relationship.sourceNodeId) ?? [];
            if (!targets.includes(relationship.targetNodeId)) targets.push(relationship.targetNodeId);
            children.set(relationship.sourceNodeId, targets);
        }
        const rows: PackageTreeRow[] = [];
        const append = (nodeId: string, depth: number, visited: Set<string>, path: string) => {
            if (visited.has(nodeId)) return;
            const object = objects.get(nodeId);
            if (!object) return;
            rows.push({ key: `${path}:${nodeId}`, depth, name: object.name, type: object.objectType });
            const nextVisited = new Set(visited).add(nodeId);
            for (const [childIndex, child] of (children.get(nodeId) ?? []).entries()) {
                append(child, depth + 1, nextVisited, `${path}.${childIndex}`);
            }
        };
        value.roots.forEach((root, rootIndex) => {
            rows.push({
                key: `root-${rootIndex}`,
                depth: 0,
                name: root.displayName,
                type: objectTypeLabel(root.kind),
            });
            root.nodeIds.forEach((nodeId, nodeIndex) => append(nodeId, 1, new Set(), `root-${rootIndex}.${nodeIndex}`));
        });
        return rows;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide package-dialog"
        class:package-import-review={!!sourceName}
        role="dialog"
        aria-modal="true"
        aria-label="Import axklib package"
        aria-busy={busy}
        use:modal={{ onescape: locked ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2>Import package</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={locked} onclick={oncancel}
                ><Icon name="close" size={15} /></button
            >
        </header>

        <div class="package-dialog-content">
            {#if !sourceName}
                <ImportSourceChoice
                    label="Package source"
                    heading="Choose a package"
                    description={`Import into ${targetName} from a configured storage location.`}
                    workspaceDetail="Choose from a configured workspace"
                    computerDetail="Choose a local package and upload it"
                    computerAvailable={desktop}
                    {onchooseworkspace}
                    {onchooselocal}
                />
            {:else}
                <section class="package-source-summary" aria-label="Selected package">
                    <div>
                        <small>Package</small>
                        <strong>{sourceName}</strong>
                    </div>
                    {#if canChangeSource}
                        <button
                            class="secondary-button"
                            type="button"
                            disabled={busy || completion.locked}
                            onclick={onchange}>Change</button
                        >
                    {/if}
                </section>

                <ImportDestinationChooser
                    mode={destinationMode}
                    partitionIndex={destinationPartitionIndex}
                    volumeName={destinationVolumeName}
                    partitions={partitionOptions}
                    volumes={volumeOptions}
                    disabled={busy || completion.locked}
                    onmode={ondestinationmode}
                    onvolume={ondestinationvolume}
                    onpartition={ondestinationpartition}
                    onname={ondestinationname}
                />

                {#if status === 'loading'}
                    <p class="dialog-progress" role="status">
                        {progress > 0 ? `Uploading package · ${Math.round(progress * 100)}%` : 'Verifying package…'}
                    </p>
                {:else if inspection}
                    <div class="package-review-grid">
                        <section class="package-tree-section" aria-label="Package contents">
                            <div class="package-section-heading">
                                <h3>Package contents</h3>
                                <small
                                    >{inspection.objects.length} objects ·
                                    {formatStoredSize(inspection.totalPayloadBytes)}</small
                                >
                            </div>
                            <div class="package-tree" role="tree">
                                {#each treeRows as row (row.key)}
                                    <div
                                        class="package-tree-row"
                                        role="treeitem"
                                        aria-selected="false"
                                        style={`--package-depth: ${row.depth}`}
                                    >
                                        <Icon
                                            name={row.type === 'Wave Data' || row.type === 'SMPL'
                                                ? 'waveform'
                                                : row.depth === 0
                                                  ? 'folder'
                                                  : 'archive'}
                                            size={13}
                                        />
                                        <strong>{row.name}</strong>
                                        <small>{objectTypeLabel(row.type)}</small>
                                    </div>
                                {/each}
                            </div>
                        </section>

                        <ImportPlanReview
                            {plan}
                            {busy}
                            {targetName}
                            {renames}
                            {programSlots}
                            {opaqueSequenceActions}
                            {onrename}
                            {onprogramslot}
                            {onprogramstart}
                            {onopaquesequenceaction}
                        />
                    </div>
                {/if}
            {/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <span class="dialog-footer-status" role="status" title={footerStatus}>{footerStatus}</span>
            <button class="secondary-button" type="button" disabled={locked} onclick={oncancel}
                >{completion.phase === 'refresh-failed' ? 'Close' : 'Cancel'}</button
            >
            {#if ['unconfirmed', 'checking', 'refresh-failed', 'refreshing'].includes(completion.phase)}
                <button
                    class="primary-button"
                    type="button"
                    disabled={completion.busy || (completion.phase === 'unconfirmed' && !completion.canCheck)}
                    onclick={onrecover}
                    >{['unconfirmed', 'checking'].includes(completion.phase) ? 'Check status' : 'Refresh'}</button
                >
            {:else if sourceName}
                <button class="secondary-button" type="button" disabled={!canCheckConflicts} onclick={onreplan}
                    >Review</button
                >
                <button
                    class="primary-button"
                    type="button"
                    disabled={!canImport}
                    title={importDisabledReason}
                    onclick={onconfirm}
                >
                    Import
                </button>
            {/if}
        </footer>
    </div>
</div>
