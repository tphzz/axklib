import { nativeFileSource } from '../../lib/nativeFileSource';
import { selectLocalVolumePackages } from '../../lib/nativePackages';
import type { ClientUploadLocation, InputFileLocation } from '../../lib/storageLocations';
import type {
    ImageSessionPackageImportPlan,
    ImageTransport,
    PackageInspection,
    PackageOpaqueSequenceDecision,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { reportError } from '../../lib/diagnostics';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackagePickerHistory } from './packagePickerHistory';

const volumePackageExtensions = new Set(['axkvol']);
const maximumBatchPackages = 256;

export interface BatchPackageItem {
    id: string;
    selected: boolean;
    source: InputFileLocation;
    sourceName: string;
    inspection: PackageInspection;
    upload: ClientUploadLocation | null;
    localPath: string | null;
}

export interface PackageBatchImportRequest {
    partition: DiskTreeItem;
    items: BatchPackageItem[];
    plan: ImageSessionPackageImportPlan | null;
    volumeNames: Record<string, string>;
    opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
    hasUnvalidatedChanges: boolean;
    status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
    completedFiles: number;
    totalFiles: number;
    progress: number;
    error: string;
}

interface PackageBatchImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    pickerHistory: PackagePickerHistory;
    isDesktop: boolean;
    sessionId: () => number | null;
    invalidateSession: (sessionId: number) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
}

export class PackageBatchImportWorkflow {
    request = $state<PackageBatchImportRequest | null>(null);
    private generation = 0;
    private abortController: AbortController | null = null;
    private plannedItemIds: string[] = [];

    constructor(private readonly dependencies: PackageBatchImportDependencies) {}

    open(partition: DiskTreeItem): void {
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        this.plannedItemIds = [];
        this.request = {
            partition,
            items: [],
            plan: null,
            volumeNames: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'choosing',
            completedFiles: 0,
            totalFiles: 0,
            progress: 0,
            error: '',
        };
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

    setSelected(itemId: string, selected: boolean): void {
        if (!this.request || this.request.status !== 'ready') return;
        const item = this.request.items.find((candidate) => candidate.id === itemId);
        if (!item || item.selected === selected) return;
        this.request = {
            ...this.request,
            items: this.request.items.map((candidate) =>
                candidate.id === itemId ? { ...candidate, selected } : candidate,
            ),
            hasUnvalidatedChanges: true,
            error: '',
        };
    }

    setAllSelected(selected: boolean): void {
        if (!this.request || this.request.status !== 'ready') return;
        if (this.request.items.every((item) => item.selected === selected)) return;
        this.request = {
            ...this.request,
            items: this.request.items.map((item) => ({ ...item, selected })),
            hasUnvalidatedChanges: true,
            error: '',
        };
    }

    opaqueSequenceAction(itemId: string, nodeId: string, action: PackageOpaqueSequenceDecision['action']): void {
        if (!this.request || this.request.status !== 'ready') return;
        const key = this.opaqueSequenceKey(itemId, nodeId);
        if (this.request.opaqueSequenceActions[key] === action) return;
        this.request = {
            ...this.request,
            opaqueSequenceActions: { ...this.request.opaqueSequenceActions, [key]: action },
            hasUnvalidatedChanges: true,
        };
        void this.replan();
    }

    destinationName(itemId: string): string {
        const request = this.request;
        if (!request) return '';
        const item = request.items.find((candidate) => candidate.id === itemId);
        if (!item) return '';
        const selectedItems = request.items.filter((candidate) => candidate.selected);
        const packageIndex = selectedItems.findIndex((candidate) => candidate.id === itemId);
        return (
            request.volumeNames[itemId] ??
            (!request.hasUnvalidatedChanges && packageIndex >= 0
                ? request.plan?.packages.find((candidate) => candidate.packageIndex === packageIndex)
                      ?.destinationVolumeName
                : undefined) ??
            item.inspection.roots[0]?.displayName ??
            ''
        );
    }

    async chooseWorkspace(): Promise<void> {
        if (!this.request) return;
        const selections = await this.dependencies.picker.chooseFiles('Choose volume packages', ['axkvol'], {
            parentDialog: 'package-import',
            initialDirectory: this.dependencies.pickerHistory.lastDirectory,
            initialFile: this.dependencies.pickerHistory.lastImportedWorkspaceFile,
            ondirectorychange: (directory) => (this.dependencies.pickerHistory.lastDirectory = directory),
        });
        if (!selections?.length || !this.request) return;
        if (selections.length > maximumBatchPackages) {
            this.request = {
                ...this.request,
                error: `Select at most ${maximumBatchPackages} volume packages at once`,
            };
            return;
        }
        await this.loadSources(
            selections.map((source) => ({ source, sourceName: source.displayName, upload: null, localPath: null })),
        );
    }

    async chooseLocal(): Promise<void> {
        if (!this.request || !this.dependencies.isDesktop) return;
        const staged: Array<{
            source: InputFileLocation;
            sourceName: string;
            upload: ClientUploadLocation;
            localPath: string;
        }> = [];
        try {
            const paths = await selectLocalVolumePackages(this.dependencies.pickerHistory.lastImportedLocalPath);
            if (!paths.length || !this.request) return;
            if (paths.length > maximumBatchPackages) {
                this.request = {
                    ...this.request,
                    error: `Select at most ${maximumBatchPackages} volume packages at once`,
                };
                return;
            }
            const controller = new AbortController();
            this.abortController?.abort();
            this.abortController = controller;
            const generation = ++this.generation;
            this.request = {
                ...this.request,
                status: 'loading',
                totalFiles: paths.length,
                completedFiles: 0,
                progress: 0,
                error: '',
            };
            for (const [index, path] of paths.entries()) {
                const file = await nativeFileSource(path, volumePackageExtensions, 'application/octet-stream');
                const upload = await this.dependencies.transport.uploadClientFile(
                    file,
                    'PACKAGE',
                    (sent, total) => {
                        if (generation === this.generation && this.request) {
                            this.request = {
                                ...this.request,
                                progress: index + (total === 0 ? 0 : sent / total),
                            };
                        }
                    },
                    controller.signal,
                );
                staged.push({ source: upload, sourceName: file.name, upload, localPath: path });
                if (generation === this.generation && this.request) {
                    this.request = { ...this.request, completedFiles: index + 1 };
                }
            }
            if (this.abortController === controller) this.abortController = null;
            if (generation !== this.generation || !this.request) {
                await Promise.all(staged.map((item) => this.releaseUpload(item.upload)));
                return;
            }
            await this.loadSources(staged, generation);
        } catch (error) {
            await Promise.all(staged.map((item) => this.releaseUpload(item.upload)));
            this.abortController = null;
            if (!this.request) return;
            reportError('Import local volume packages failed', error);
            this.request = { ...this.request, status: 'choosing', error: userFacingMessage(error) };
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
            request.partition.partitionIndex === undefined
        ) {
            return;
        }
        const generation = ++this.generation;
        let jobStarted = false;
        this.request = { ...request, status: 'applying', error: '' };
        this.dependencies.setStatus(`Importing ${selectedItems.length} volume packages`);
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
                partitionIndex: request.partition.partitionIndex,
                volumeName: request.plan.packages[0]?.destinationVolumeName,
            });
            this.dependencies.setStatus(`Imported ${selectedItems.length} volume packages`);
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
            await this.dependencies.refreshSession({ partitionIndex: request.partition.partitionIndex! });
            if (generation !== this.generation || !this.request) return;
            await this.plan(generation);
            if (generation === this.generation && this.request?.status === 'ready' && this.request.plan) {
                this.dependencies.setStatus('Image changed; import conflicts checked again');
            }
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
                partitionIndex: request.partition.partitionIndex!,
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

    private async loadSources(
        sources: Array<{
            source: InputFileLocation;
            sourceName: string;
            upload: ClientUploadLocation | null;
            localPath: string | null;
        }>,
        existingGeneration?: number,
    ): Promise<void> {
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
                this.validateInspection(source.sourceName, inspection);
                items.push({ ...source, id: `batch-${generation}-${index}`, selected: true, inspection });
                if (generation !== this.generation || !this.request) {
                    await Promise.all(sources.map((item) => this.releaseUpload(item.upload)));
                    return;
                }
                this.request = { ...this.request, items: [...items], completedFiles: index + 1 };
            }
            this.request = { ...this.request, status: 'planning' };
            await this.plan(generation);
        } catch (error) {
            await Promise.all(sources.map((item) => this.releaseUpload(item.upload)));
            if (generation !== this.generation || !this.request) return;
            this.request = { ...this.request, items: [], status: 'choosing', error: userFacingMessage(error) };
        }
    }

    private validateInspection(sourceName: string, inspection: PackageInspection): void {
        if (
            !inspection.valid ||
            inspection.packageKind !== 'VOLUME' ||
            inspection.requiredExtension.toLocaleLowerCase() !== '.axkvol' ||
            inspection.roots.length !== 1 ||
            inspection.roots[0]?.kind !== 'VOLUME'
        ) {
            throw new Error(`${sourceName} is not a valid single-volume .axkvol package`);
        }
    }

    private async plan(generation: number, replacePlanToken?: string): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        const partitionIndex = request?.partition.partitionIndex;
        const selectedItems = request?.items.filter((item) => item.selected) ?? [];
        if (!request || selectedItems.length === 0 || sessionId === null || partitionIndex === undefined) return;
        this.request = { ...request, status: 'planning', error: '' };
        try {
            const volumeNameOverrides = selectedItems
                .map((item, packageIndex) => ({
                    packageIndex,
                    volumeName: request.volumeNames[item.id]?.trim() ?? '',
                }))
                .filter((item) => item.volumeName.length > 0);
            const opaqueSequenceDecisions = selectedItems.flatMap((item, packageIndex) => {
                const prefix = `${item.id}:`;
                return Object.entries(request.opaqueSequenceActions)
                    .filter(([key]) => key.startsWith(prefix))
                    .map(([key, action]) => ({ packageIndex, nodeId: key.slice(prefix.length), action }));
            });
            const plan = await this.dependencies.transport.planImagePackageImport(
                sessionId,
                selectedItems.map((item) => item.source),
                { kind: 'CREATE_VOLUMES_FROM_HINTS', partitionIndex, volumeNameOverrides },
                [],
                [],
                replacePlanToken,
                opaqueSequenceDecisions,
            );
            if (generation !== this.generation || !this.request) {
                await this.releasePlan(plan);
                return;
            }
            this.request = {
                ...this.request,
                plan,
                volumeNames: {
                    ...request.volumeNames,
                    ...Object.fromEntries(
                        plan.packages.flatMap((item) => {
                            const selectedItem = selectedItems[item.packageIndex];
                            return selectedItem
                                ? [
                                      [
                                          selectedItem.id,
                                          request.volumeNames[selectedItem.id] ?? item.destinationVolumeName,
                                      ],
                                  ]
                                : [];
                        }),
                    ),
                },
                hasUnvalidatedChanges: false,
                status: 'ready',
                error: '',
            };
            this.plannedItemIds = selectedItems.map((item) => item.id);
        } catch (error) {
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: userFacingMessage(error) };
            }
        }
    }

    private opaqueSequenceKey(itemId: string, nodeId: string): string {
        return `${itemId}:${nodeId}`;
    }

    private async releasePlan(plan: ImageSessionPackageImportPlan | null): Promise<void> {
        if (plan)
            await this.dependencies.transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
    }

    private async releaseUpload(upload: ClientUploadLocation | null): Promise<void> {
        if (upload) await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
    }
}
