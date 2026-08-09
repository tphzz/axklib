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
    volumeNames: Record<number, string>;
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

    constructor(private readonly dependencies: PackageBatchImportDependencies) {}

    open(partition: DiskTreeItem): void {
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
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

    renameVolume(packageIndex: number, name: string): void {
        if (!this.request || this.request.status !== 'ready') return;
        const current = this.destinationName(packageIndex);
        if (current === name) return;
        this.request = {
            ...this.request,
            volumeNames: { ...this.request.volumeNames, [packageIndex]: name },
            hasUnvalidatedChanges: true,
        };
    }

    opaqueSequenceAction(packageIndex: number, nodeId: string, action: PackageOpaqueSequenceDecision['action']): void {
        if (!this.request || this.request.status !== 'ready') return;
        const key = this.opaqueSequenceKey(packageIndex, nodeId);
        if (this.request.opaqueSequenceActions[key] === action) return;
        this.request = {
            ...this.request,
            opaqueSequenceActions: { ...this.request.opaqueSequenceActions, [key]: action },
            hasUnvalidatedChanges: true,
        };
        void this.replan();
    }

    destinationName(packageIndex: number): string {
        return (
            this.request?.volumeNames[packageIndex] ??
            this.request?.plan?.packages.find((item) => item.packageIndex === packageIndex)?.destinationVolumeName ??
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

    async remove(packageIndex: number): Promise<void> {
        const request = this.request;
        if (!request || request.status === 'applying' || !request.items[packageIndex]) return;
        const removed = request.items[packageIndex];
        ++this.generation;
        await this.releasePlan(request.plan);
        await this.releaseUpload(removed.upload);
        const items = request.items.filter((_, index) => index !== packageIndex);
        this.request = {
            ...request,
            items,
            plan: null,
            volumeNames: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: items.length ? 'planning' : 'choosing',
            error: '',
        };
        if (items.length) await this.plan(++this.generation);
    }

    async replan(): Promise<void> {
        if (!this.request?.items.length || this.request.status === 'applying') return;
        await this.plan(++this.generation, this.request.plan?.planToken);
    }

    async apply(): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (
            !request?.plan?.valid ||
            request.hasUnvalidatedChanges ||
            sessionId === null ||
            request.partition.partitionIndex === undefined
        ) {
            return;
        }
        const generation = ++this.generation;
        this.request = { ...request, status: 'applying', error: '' };
        this.dependencies.setStatus(`Importing ${request.items.length} volume packages`);
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startImagePackageImport(request.plan!.planToken),
                (update) => update.progress?.label && this.dependencies.setStatus(update.progress.label),
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Package import did not complete');
            const last = request.items.at(-1);
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
            this.dependencies.setStatus(`Imported ${request.items.length} volume packages`);
        } catch (error) {
            const message = userFacingMessage(error);
            this.dependencies.setStatus(message);
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: message };
            }
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
                items.push({ ...source, inspection });
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
        if (!request?.items.length || sessionId === null || partitionIndex === undefined) return;
        this.request = { ...request, status: 'planning', error: '' };
        try {
            const volumeNameOverrides = Object.entries(request.volumeNames)
                .map(([packageIndex, volumeName]) => ({
                    packageIndex: Number(packageIndex),
                    volumeName: volumeName.trim(),
                }))
                .filter((item) => item.volumeName.length > 0);
            const opaqueSequenceDecisions = Object.entries(request.opaqueSequenceActions).map(([key, action]) => {
                const separator = key.indexOf(':');
                return { packageIndex: Number(key.slice(0, separator)), nodeId: key.slice(separator + 1), action };
            });
            const plan = await this.dependencies.transport.planImagePackageImport(
                sessionId,
                request.items.map((item) => item.source),
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
                volumeNames: Object.fromEntries(
                    plan.packages.map((item) => [
                        item.packageIndex,
                        request.volumeNames[item.packageIndex] ?? item.destinationVolumeName,
                    ]),
                ),
                hasUnvalidatedChanges: false,
                status: 'ready',
                error: '',
            };
        } catch (error) {
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: userFacingMessage(error) };
            }
        }
    }

    private opaqueSequenceKey(packageIndex: number, nodeId: string): string {
        return `${packageIndex}:${nodeId}`;
    }

    private async releasePlan(plan: ImageSessionPackageImportPlan | null): Promise<void> {
        if (plan)
            await this.dependencies.transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
    }

    private async releaseUpload(upload: ClientUploadLocation | null): Promise<void> {
        if (upload) await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
    }
}
