import { selectLocalPackages } from '../../lib/nativePackages';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { packageImportExtensions } from '../../lib/packageImportMedia';
import type { ClientUploadLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, PackageOpaqueSequenceDecision } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { reportError } from '../../lib/diagnostics';
import { userFacingMessage } from '../../lib/userFacingMessage';
import {
    collectImportDestinations,
    initialImportDestination,
    type ImportDestinationMode,
    type ImportPartitionOption,
    type ImportVolumeOption,
} from './packageDestinations';
import {
    batchDecisionKey,
    batchDestinationName,
    batchPlanArguments,
    mergeBatchPlanSuggestions,
    normalizedBatchDestination,
    separateVolumesAvailable,
    suggestedSharedVolumeName,
} from './packageBatchPlanning';
import { uploadDroppedPackageSources, uploadLocalPackageSources, type BatchPackageSource } from './packageBatchSources';
import type {
    BatchPackageItem,
    PackageBatchImportDependencies,
    PackageBatchDestinationStrategy,
    PackageBatchImportRequest,
} from './packageBatchTypes';

const maximumBatchPackages = 256;

export class PackageBatchImportWorkflow {
    request = $state<PackageBatchImportRequest | null>(null);
    private generation = 0;
    private abortController: AbortController | null = null;
    private plannedItemIds: string[] = [];

    constructor(private readonly dependencies: PackageBatchImportDependencies) {}

    dropAvailable(): boolean {
        return this.dependencies.mutationsAvailable?.() ?? false;
    }

    partitionOptions(): ImportPartitionOption[] {
        return this.destinations().partitions;
    }

    volumeOptions(): ImportVolumeOption[] {
        return this.destinations().volumes;
    }

    canUseSeparateVolumes(): boolean {
        return this.request ? separateVolumesAvailable(this.request.items.filter((item) => item.selected)) : false;
    }

    open(item: DiskTreeItem | null): void {
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        this.plannedItemIds = [];
        const destination = initialImportDestination(item) ?? {
            mode: 'existing' as const,
            partitionIndex: this.destinations().partitions[0]?.partitionIndex ?? null,
            volumeName: '',
        };
        this.request = {
            item,
            canChangeSources: true,
            items: [],
            plan: null,
            destinationStrategy: item?.kind === 'partition' ? 'separate' : 'shared',
            destinationMode: destination.mode,
            destinationPartitionIndex: destination.partitionIndex,
            destinationVolumeName: destination.volumeName,
            volumeNames: {},
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'choosing',
            completedFiles: 0,
            totalFiles: 0,
            progress: 0,
            error: '',
        };
    }

    setDestinationStrategy(strategy: PackageBatchDestinationStrategy): void {
        const request = this.request;
        if (!request || request.status === 'applying' || request.destinationStrategy === strategy) return;
        if (strategy === 'separate' && !separateVolumesAvailable(request.items.filter((item) => item.selected))) return;
        this.updateDestination({ destinationStrategy: strategy });
    }

    setDestinationMode(mode: ImportDestinationMode): void {
        const request = this.request;
        if (!request || request.status === 'applying' || request.destinationMode === mode) return;
        const destinations = this.destinations();
        this.updateDestination({
            destinationStrategy: 'shared',
            destinationMode: mode,
            destinationPartitionIndex:
                request.destinationPartitionIndex ?? destinations.partitions[0]?.partitionIndex ?? null,
            destinationVolumeName: mode === 'existing' ? '' : suggestedSharedVolumeName(request.items),
        });
    }

    setExistingVolume(partitionIndex: number | null, volumeName: string): void {
        const request = this.request;
        if (!request || request.status === 'applying') return;
        this.updateDestination({
            destinationStrategy: 'shared',
            destinationMode: 'existing',
            destinationPartitionIndex: partitionIndex,
            destinationVolumeName: volumeName,
        });
    }

    setDestinationPartition(partitionIndex: number): void {
        const request = this.request;
        if (!request || request.status === 'applying') return;
        this.updateDestination({
            destinationPartitionIndex: partitionIndex,
            ...(request.destinationStrategy === 'shared' && request.destinationMode === 'existing'
                ? { destinationVolumeName: '' }
                : {}),
        });
    }

    setDestinationVolumeName(volumeName: string): void {
        const request = this.request;
        if (!request || request.status === 'applying') return;
        this.updateDestination({ destinationVolumeName: volumeName.slice(0, 16) });
    }

    renameVolume(itemId: string, name: string): void {
        if (!this.request || this.request.status !== 'ready') return;
        const current = this.destinationName(itemId);
        if (current === name) return;
        this.request = {
            ...this.request,
            volumeNames: { ...this.request.volumeNames, [itemId]: name },
            hasUnvalidatedChanges: true,
        };
    }

    rename(itemId: string, nodeId: string, name: string): void {
        if (!this.request || this.request.status !== 'ready') return;
        const key = batchDecisionKey(itemId, nodeId);
        if (this.request.renames[key] === name) return;
        this.request = {
            ...this.request,
            renames: { ...this.request.renames, [key]: name },
            hasUnvalidatedChanges: true,
        };
    }

    programSlot(itemId: string, nodeId: string, slot: number): void {
        if (!this.request || this.request.status !== 'ready' || !Number.isInteger(slot) || slot < 1 || slot > 128) {
            return;
        }
        const key = batchDecisionKey(itemId, nodeId);
        if (this.request.programSlots[key] === slot) return;
        this.request = {
            ...this.request,
            programSlots: { ...this.request.programSlots, [key]: slot },
            hasUnvalidatedChanges: true,
        };
    }

    programStart(placementId: string, start: number): void {
        const request = this.request;
        if (!request || request.status !== 'ready' || !Number.isInteger(start)) return;
        const placement = request.plan?.programSlotPlacements.find(
            (candidate) => candidate.placementId === placementId,
        );
        if (
            !placement ||
            placement.mode !== 'CONTIGUOUS' ||
            start < 1 ||
            start + placement.requiredSlotCount - 1 > 128
        ) {
            return;
        }
        const selectedItems = request.items.filter((item) => item.selected);
        const programSlots = { ...request.programSlots };
        for (const [offset, mapping] of placement.mappings.entries()) {
            const item = selectedItems[mapping.packageIndex];
            if (item) programSlots[batchDecisionKey(item.id, mapping.nodeId)] = start + offset;
        }
        this.request = { ...request, programSlots, hasUnvalidatedChanges: true };
    }

    setSelected(itemId: string, selected: boolean): void {
        if (!this.request || this.request.status !== 'ready') return;
        const item = this.request.items.find((candidate) => candidate.id === itemId);
        if (!item || item.selected === selected) return;
        const items = this.request.items.map((candidate) =>
            candidate.id === itemId ? { ...candidate, selected } : candidate,
        );
        this.request = {
            ...this.request,
            items,
            destinationStrategy:
                this.request.destinationStrategy === 'separate' &&
                !separateVolumesAvailable(items.filter((entry) => entry.selected))
                    ? 'shared'
                    : this.request.destinationStrategy,
            hasUnvalidatedChanges: true,
            error: '',
        };
    }

    setAllSelected(selected: boolean): void {
        if (!this.request || this.request.status !== 'ready') return;
        if (this.request.items.every((item) => item.selected === selected)) return;
        const items = this.request.items.map((item) => ({ ...item, selected }));
        this.request = {
            ...this.request,
            items,
            destinationStrategy:
                this.request.destinationStrategy === 'separate' &&
                !separateVolumesAvailable(items.filter((entry) => entry.selected))
                    ? 'shared'
                    : this.request.destinationStrategy,
            hasUnvalidatedChanges: true,
            error: '',
        };
    }

    opaqueSequenceAction(itemId: string, nodeId: string, action: PackageOpaqueSequenceDecision['action']): void {
        if (!this.request || this.request.status !== 'ready') return;
        const key = batchDecisionKey(itemId, nodeId);
        if (this.request.opaqueSequenceActions[key] === action) return;
        this.request = {
            ...this.request,
            opaqueSequenceActions: { ...this.request.opaqueSequenceActions, [key]: action },
            hasUnvalidatedChanges: true,
        };
    }

    destinationName(itemId: string): string {
        return this.request ? batchDestinationName(this.request, itemId) : '';
    }

    async chooseWorkspace(): Promise<void> {
        if (!this.request) return;
        const selections = await this.dependencies.picker.chooseFiles(
            'Choose axklib packages',
            [...packageImportExtensions],
            {
                parentDialog: 'package-import',
                initialDirectory: this.dependencies.pickerHistory.lastDirectory,
                initialFile: this.dependencies.pickerHistory.lastImportedWorkspaceFile,
                ondirectorychange: (directory) => (this.dependencies.pickerHistory.lastDirectory = directory),
            },
        );
        if (!selections?.length || !this.request) return;
        if (selections.length > maximumBatchPackages) {
            this.request = {
                ...this.request,
                error: `Select at most ${maximumBatchPackages} packages at once`,
            };
            return;
        }
        await this.loadSources(
            selections.map((source) => ({ source, sourceName: source.displayName, upload: null, localPath: null })),
        );
    }

    async chooseLocal(closeOnCancel = false): Promise<void> {
        if (!this.request || !this.dependencies.isDesktop) return;
        try {
            const paths = await selectLocalPackages(this.dependencies.pickerHistory.lastImportedLocalPath);
            if (!paths.length) {
                if (closeOnCancel) await this.close();
                return;
            }
            if (!this.request) return;
            if (paths.length > maximumBatchPackages) {
                this.request = {
                    ...this.request,
                    error: `Select at most ${maximumBatchPackages} packages at once`,
                };
                return;
            }
            await this.stageSources(paths.length, (signal, progress) =>
                uploadLocalPackageSources(this.dependencies.transport, paths, signal, { progress }),
            );
        } catch (error) {
            if (!this.request) return;
            reportError('Import local packages failed', error);
            this.request = { ...this.request, status: 'choosing', error: userFacingMessage(error) };
        }
    }

    async requestDroppedFiles(files: ClientUploadSource[], item: DiskTreeItem | null): Promise<void> {
        if (!this.dropAvailable() || files.length === 0) return;
        this.open(item);
        if (files.length > maximumBatchPackages) {
            this.request = {
                ...this.request!,
                canChangeSources: false,
                error: `Drop at most ${maximumBatchPackages} packages at once`,
            };
            return;
        }
        this.request = {
            ...this.request!,
            canChangeSources: false,
        };
        try {
            await this.stageSources(files.length, (signal, progress) =>
                uploadDroppedPackageSources(this.dependencies.transport, files, signal, { progress }),
            );
        } catch (error) {
            if (this.request) {
                this.request = { ...this.request, status: 'choosing', error: userFacingMessage(error) };
            }
        }
    }

    private async stageSources(
        totalFiles: number,
        load: (
            signal: AbortSignal,
            progress: (completedFiles: number, currentProgress: number) => void,
        ) => Promise<BatchPackageSource[]>,
    ): Promise<void> {
        if (!this.request) return;
        const controller = new AbortController();
        this.abortController?.abort();
        this.abortController = controller;
        const generation = ++this.generation;
        let staged: BatchPackageSource[] = [];
        this.request = {
            ...this.request,
            status: 'loading',
            totalFiles,
            completedFiles: 0,
            progress: 0,
            error: '',
        };
        try {
            staged = await load(controller.signal, (completedFiles, currentProgress) => {
                if (generation !== this.generation || !this.request) return;
                this.request = { ...this.request, completedFiles, progress: completedFiles + currentProgress };
            });
            if (this.abortController === controller) this.abortController = null;
            if (generation !== this.generation || !this.request) {
                await Promise.all(staged.map((source) => this.releaseUpload(source.upload)));
                return;
            }
            await this.loadSources(staged, generation);
        } catch (error) {
            await Promise.all(staged.map((source) => this.releaseUpload(source.upload)));
            if (this.abortController === controller) this.abortController = null;
            throw error;
        }
    }

    async replan(): Promise<void> {
        const request = this.request;
        if (!request?.items.some((item) => item.selected) || request.status === 'applying') return;
        const selectedItemIds = request.items.filter((item) => item.selected).map((item) => item.id);
        const canReplacePlan =
            request.plan !== null &&
            selectedItemIds.length === this.plannedItemIds.length &&
            selectedItemIds.every((itemId, index) => itemId === this.plannedItemIds[index]);
        const generation = ++this.generation;
        if (!canReplacePlan && request.plan) {
            this.request = { ...request, status: 'planning', error: '' };
            await this.releasePlan(request.plan);
            if (generation !== this.generation || !this.request) return;
            this.request = { ...this.request, plan: null };
            this.plannedItemIds = [];
        }
        await this.plan(generation, canReplacePlan ? request.plan?.planToken : undefined);
    }

    async apply(): Promise<void> {
        const request = this.request;
        const selectedItems = request?.items.filter((item) => item.selected) ?? [];
        const sessionId = this.dependencies.sessionId();
        if (
            !request?.plan?.valid ||
            request.hasUnvalidatedChanges ||
            selectedItems.length === 0 ||
            sessionId === null ||
            request.destinationPartitionIndex === null
        ) {
            return;
        }
        const generation = ++this.generation;
        let jobStarted = false;
        this.request = { ...request, status: 'applying', error: '' };
        this.dependencies.setStatus(`Importing ${selectedItems.length} packages`);
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startImagePackageImport(request.plan!.planToken),
                (update) => update.progress?.label && this.dependencies.setStatus(update.progress.label),
                () => {
                    jobStarted = true;
                },
            );
            if (completed.status !== 'completed') {
                if (this.isStalePlanError(completed.errorCode)) {
                    await this.recoverStalePlan(request, generation);
                    return;
                }
                const message = completed.error ?? 'Package import did not complete';
                this.dependencies.setStatus(message);
                if (generation === this.generation && this.request) {
                    this.request = { ...this.request, status: 'ready', error: message };
                }
                return;
            }
            const last = selectedItems.at(-1);
            if (last?.source.kind === 'server-file') {
                this.dependencies.pickerHistory.lastImportedWorkspaceFile = last.source.reference;
            } else if (last?.localPath) {
                this.dependencies.pickerHistory.lastImportedLocalPath = last.localPath;
            }
            await Promise.all(request.items.map((item) => this.releaseUpload(item.upload)));
            this.request = null;
            await this.dependencies.refreshSession({
                partitionIndex: request.destinationPartitionIndex,
                volumeName: request.plan.packages[0]?.destinationVolumeName,
            });
            this.dependencies.setStatus(`Imported ${selectedItems.length} packages`);
        } catch (error) {
            if (jobStarted) {
                await this.recoverUncertainApply(request, generation);
                return;
            }
            const message = userFacingMessage(error);
            this.dependencies.setStatus(message);
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: message };
            }
        }
    }

    private isStalePlanError(errorCode: string | undefined): boolean {
        return errorCode === 'image_revision_stale' || errorCode === 'package_plan_stale';
    }

    private async recoverStalePlan(request: PackageBatchImportRequest, generation: number): Promise<void> {
        if (generation !== this.generation || !this.request) return;
        this.request = {
            ...request,
            plan: null,
            hasUnvalidatedChanges: true,
            status: 'planning',
            error: '',
        };
        this.plannedItemIds = [];
        await this.releasePlan(request.plan);
        try {
            await this.dependencies.refreshSession({ partitionIndex: request.destinationPartitionIndex! });
            if (generation !== this.generation || !this.request) return;
            this.request = { ...this.request, status: 'ready' };
            this.dependencies.setStatus('Image changed; check import conflicts again');
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, status: 'ready', error: message };
            this.dependencies.setStatus(message);
        }
    }

    private async recoverUncertainApply(request: PackageBatchImportRequest, generation: number): Promise<void> {
        if (generation !== this.generation) return;
        this.request = null;
        this.plannedItemIds = [];
        await Promise.all(request.items.map((item) => this.releaseUpload(item.upload)));
        const message = 'Import completion could not be confirmed; review the refreshed image before retrying';
        try {
            await this.dependencies.refreshSession({
                partitionIndex: request.destinationPartitionIndex!,
                volumeName: request.plan?.packages[0]?.destinationVolumeName,
            });
            this.dependencies.setStatus(message);
        } catch (error) {
            reportError('Refresh after uncertain package import failed', error);
            this.dependencies.setStatus(`${message}. Refresh failed: ${userFacingMessage(error)}`);
        }
    }

    async close(): Promise<void> {
        if (this.request?.status === 'applying') return;
        const request = this.request;
        this.request = null;
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        if (!request) return;
        this.plannedItemIds = [];
        await this.releasePlan(request.plan);
        await Promise.all(request.items.map((item) => this.releaseUpload(item.upload)));
    }

    private async loadSources(sources: BatchPackageSource[], existingGeneration?: number): Promise<void> {
        if (!this.request) return;
        const previous = this.request;
        await this.releasePlan(previous.plan);
        await Promise.all(previous.items.map((item) => this.releaseUpload(item.upload)));
        this.plannedItemIds = [];
        const generation = existingGeneration ?? ++this.generation;
        this.request = {
            ...previous,
            items: [],
            plan: null,
            volumeNames: {},
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'loading',
            completedFiles: 0,
            totalFiles: sources.length,
            progress: 0,
            error: '',
        };
        const items: BatchPackageItem[] = [];
        try {
            for (const [index, source] of sources.entries()) {
                const inspection = await this.dependencies.transport.inspectPackage(source.source, false);
                if (!inspection.valid) {
                    throw new Error(`${source.sourceName} is not a valid portable package or A3K archive`);
                }
                items.push({ ...source, id: `batch-${generation}-${index}`, selected: true, inspection });
                if (generation !== this.generation || !this.request) {
                    await Promise.all(sources.map((item) => this.releaseUpload(item.upload)));
                    return;
                }
                this.request = { ...this.request, items: [...items], completedFiles: index + 1 };
            }
            const destination = normalizedBatchDestination(this.request, items, this.destinations());
            this.request = {
                ...this.request,
                ...destination,
                status: 'ready',
                hasUnvalidatedChanges: true,
            };
        } catch (error) {
            await Promise.all(sources.map((item) => this.releaseUpload(item.upload)));
            if (generation !== this.generation || !this.request) return;
            reportError('Inspect package batch failed', error);
            this.request = { ...this.request, items: [], status: 'choosing', error: userFacingMessage(error) };
        }
    }

    private async plan(generation: number, replacePlanToken?: string): Promise<void> {
        try {
            const automaticPlanToken = await this.planOnce(generation, replacePlanToken, true);
            if (automaticPlanToken) await this.planOnce(generation, automaticPlanToken, false);
        } catch (error) {
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: userFacingMessage(error) };
            }
        }
    }

    private async planOnce(
        generation: number,
        replacePlanToken: string | undefined,
        allowAutomaticProgramSlotCheck: boolean,
    ): Promise<string | null> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        const selectedItems = request?.items.filter((item) => item.selected) ?? [];
        const arguments_ = request ? batchPlanArguments(request, selectedItems) : null;
        if (!request || selectedItems.length === 0 || sessionId === null || !arguments_) {
            if (request && generation === this.generation) {
                this.request = { ...request, status: 'ready', hasUnvalidatedChanges: true };
            }
            return null;
        }
        if (generation !== this.generation) return null;
        this.request = { ...request, status: 'planning', error: '' };
        const plan = await this.dependencies.transport.planImagePackageImport(
            sessionId,
            selectedItems.map((item) => item.source),
            arguments_.destination,
            arguments_.renames,
            arguments_.programSlotAssignments,
            replacePlanToken,
            arguments_.opaqueSequenceDecisions,
        );
        if (generation !== this.generation || !this.request) {
            await this.releasePlan(plan);
            return null;
        }
        const merged = mergeBatchPlanSuggestions(request, selectedItems, plan);
        const checkSuggestedProgramSlots =
            allowAutomaticProgramSlotCheck &&
            merged.suggestedSlotsAdded &&
            plan.programSlotPlacements.some(
                (placement) => !placement.applied && placement.mode !== 'UNAVAILABLE' && placement.mappings.length > 0,
            );
        this.request = {
            ...this.request,
            plan,
            volumeNames: merged.volumeNames,
            renames: merged.renames,
            programSlots: merged.programSlots,
            hasUnvalidatedChanges: merged.suggestedSlotsAdded,
            status: checkSuggestedProgramSlots ? 'planning' : 'ready',
            error: '',
        };
        this.plannedItemIds = selectedItems.map((item) => item.id);
        return checkSuggestedProgramSlots ? plan.planToken : null;
    }

    private updateDestination(
        update: Partial<
            Pick<
                PackageBatchImportRequest,
                'destinationStrategy' | 'destinationMode' | 'destinationPartitionIndex' | 'destinationVolumeName'
            >
        >,
    ): void {
        const request = this.request;
        if (!request || request.status === 'applying') return;
        const previousPlan = request.plan;
        const next = {
            ...request,
            ...update,
            plan: null,
            renames: {},
            programSlots: {},
            hasUnvalidatedChanges: true,
            status: request.items.length > 0 ? ('ready' as const) : request.status,
            error: '',
        };
        ++this.generation;
        this.plannedItemIds = [];
        this.request = next;
        void this.releasePlan(previousPlan);
    }

    private destinations() {
        return collectImportDestinations(this.dependencies.sourceItems?.() ?? []);
    }

    private async releasePlan(plan: ImageSessionPackageImportPlan | null): Promise<void> {
        if (plan)
            await this.dependencies.transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
    }

    private async releaseUpload(upload: ClientUploadLocation | null): Promise<void> {
        if (upload) await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
    }
}
