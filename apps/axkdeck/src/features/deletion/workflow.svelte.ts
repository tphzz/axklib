import { maximumPackageExportRoots, type PackageExportSelectionState } from '../../lib/objectSelection';
import type { ObjectDeletionInspection, ImageTransport, WaveDataOrphanInspection } from '../../lib/transport';
import type { DiskTreeItem, PackageExportObject } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { JobController } from '../jobs/actions';

export interface ObjectDeletionRequest {
    targets: PackageExportObject[];
    cleanupObjectIds: string[];
    inspection: ObjectDeletionInspection | null;
    loading: boolean;
    busy: boolean;
    error: string;
}

export interface WaveDataCleanupRequest {
    volumeId: string;
    volumeName: string;
    inspection: WaveDataOrphanInspection | null;
    selectedObjectIds: string[];
    loading: boolean;
    busy: boolean;
    error: string;
}

interface DeletionWorkflowDependencies {
    transport: ImageTransport;
    jobs: JobController;
    sessionId: () => number | null;
    activeVolumeId: () => string;
    selectedSource: () => DiskTreeItem;
    refreshSession: (preferred?: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    stopPlayback: () => Promise<void>;
    selection: () => PackageExportSelectionState;
    setSelection: (selection: PackageExportSelectionState) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class DeletionWorkflow {
    objectRequest = $state<ObjectDeletionRequest | null>(null);
    cleanupRequest = $state<WaveDataCleanupRequest | null>(null);
    private objectGeneration = 0;
    private cleanupGeneration = 0;

    constructor(private readonly dependencies: DeletionWorkflowDependencies) {}

    dispose(): void {
        ++this.objectGeneration;
        ++this.cleanupGeneration;
        this.objectRequest = null;
        this.cleanupRequest = null;
    }

    requestObjects(targets: PackageExportObject[]): void {
        if (this.dependencies.sessionId() === null || targets.length === 0) return;
        const generation = ++this.objectGeneration;
        this.objectRequest = {
            targets,
            cleanupObjectIds: [],
            inspection: null,
            loading: true,
            busy: false,
            error: '',
        };
        void this.dependencies.stopPlayback();
        void this.inspectObjects(generation);
    }

    cancelObjects(): void {
        if (this.objectRequest?.busy) return;
        ++this.objectGeneration;
        this.objectRequest = null;
    }

    updateObjectSelection(objectId: string, selected: boolean): void {
        const request = this.objectRequest;
        const inspection = request?.inspection;
        if (!request || !inspection || request.busy) return;
        const included = new Set(
            inspection.impacts
                .filter((impact) => impact.role === 'DEPENDENCY' && impact.selected)
                .map((impact) => impact.objectId),
        );
        if (selected) {
            if (request.targets.length + included.size >= maximumPackageExportRoots) {
                this.dependencies.setStatus(
                    `Deletion is limited to ${maximumPackageExportRoots} targets and cleanup objects`,
                );
                return;
            }
            included.add(objectId);
        } else {
            included.delete(objectId);
            let changed = true;
            while (changed) {
                changed = false;
                for (const impact of inspection.impacts) {
                    if (
                        included.has(impact.objectId) &&
                        impact.prerequisiteObjectIds.some((prerequisite) =>
                            inspection.impacts.some(
                                (candidate) =>
                                    candidate.objectId === prerequisite &&
                                    candidate.role === 'DEPENDENCY' &&
                                    !included.has(prerequisite),
                            ),
                        )
                    ) {
                        included.delete(impact.objectId);
                        changed = true;
                    }
                }
            }
        }
        const generation = ++this.objectGeneration;
        this.objectRequest = { ...request, cleanupObjectIds: [...included], loading: true, error: '' };
        void this.inspectObjects(generation);
    }

    updateAllObjectDependencies(selected: boolean): void {
        const request = this.objectRequest;
        const inspection = request?.inspection;
        if (!request || !inspection || request.busy) return;
        const cleanupCapacity = Math.max(0, maximumPackageExportRoots - request.targets.length);
        const optional = inspection.impacts.filter(
            (impact) => impact.role === 'DEPENDENCY' && impact.status === 'OPTIONAL',
        );
        if (selected && optional.length > cleanupCapacity) {
            this.dependencies.setStatus(
                `Deletion is limited to ${maximumPackageExportRoots} targets and cleanup objects`,
            );
        }
        const generation = ++this.objectGeneration;
        this.objectRequest = {
            ...request,
            cleanupObjectIds: selected ? optional.map((impact) => impact.objectId).slice(0, cleanupCapacity) : [],
            loading: true,
            error: '',
        };
        void this.inspectObjects(generation);
    }

    async submitObjects(): Promise<void> {
        const request = this.objectRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection?.canApply || request.loading || request.busy || sessionId === null) return;
        const generation = this.objectGeneration;
        const preferred = this.currentSourcePreference();
        const started = performance.now();
        this.objectRequest = { ...request, busy: true, error: '' };
        this.dependencies.setStatus(
            `Deleting ${request.inspection.selectedObjectIds.length} ${
                request.inspection.selectedObjectIds.length === 1 ? 'object' : 'objects'
            }`,
        );
        try {
            await this.dependencies.invalidateSession(sessionId);
            const finalInspection = await this.dependencies.transport.inspectObjectDeletion(
                sessionId,
                request.targets.map((target) => target.objectId),
                request.cleanupObjectIds,
            );
            if (!finalInspection.canApply) {
                this.objectRequest = { ...request, inspection: finalInspection, busy: false };
                this.dependencies.setStatus('Deletion is blocked; review the affected references');
                return;
            }
            if (deletionInspectionFingerprint(finalInspection) !== deletionInspectionFingerprint(request.inspection)) {
                this.objectRequest = {
                    ...request,
                    inspection: finalInspection,
                    cleanupObjectIds: finalInspection.impacts
                        .filter((impact) => impact.role === 'DEPENDENCY' && impact.selected)
                        .map((impact) => impact.objectId),
                    busy: false,
                    error: 'The deletion impact changed. Review the affected objects before confirming again.',
                };
                this.dependencies.setStatus('Deletion impact changed; review before confirming again');
                return;
            }
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startObjectDeletion(
                        sessionId,
                        request.targets.map((target) => target.objectId),
                        request.cleanupObjectIds,
                    ),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Object deletion did not complete');
            }
            await this.dependencies.refreshSession(preferred);
            this.removeDeletedFromSelection(finalInspection.selectedObjectIds);
            ++this.objectGeneration;
            this.objectRequest = null;
            this.dependencies.reportTiming('delete-object', started, finalInspection.selectedObjectIds.length);
        } catch (error) {
            await this.recoverObjectFailure(error, request, sessionId, generation, preferred);
        }
    }

    requestCleanup(): void {
        const sessionId = this.dependencies.sessionId();
        const volumeId = this.dependencies.activeVolumeId();
        const selectedSource = this.dependencies.selectedSource();
        if (sessionId === null || volumeId === '' || selectedSource.kind !== 'volume') return;
        const generation = ++this.cleanupGeneration;
        this.cleanupRequest = {
            volumeId,
            volumeName: selectedSource.name,
            inspection: null,
            selectedObjectIds: [],
            loading: true,
            busy: false,
            error: '',
        };
        void this.dependencies.stopPlayback();
        void this.inspectCleanup(generation);
    }

    cancelCleanup(): void {
        if (this.cleanupRequest?.busy) return;
        this.resetCleanup();
    }

    resetCleanup(): void {
        ++this.cleanupGeneration;
        this.cleanupRequest = null;
    }

    updateCleanupSelection(objectId: string, selected: boolean): void {
        const request = this.cleanupRequest;
        if (!request || request.loading || request.busy) return;
        const selectedIds = new Set(request.selectedObjectIds);
        if (selected) selectedIds.add(objectId);
        else selectedIds.delete(objectId);
        this.cleanupRequest = { ...request, selectedObjectIds: [...selectedIds] };
    }

    updateAllCleanup(selected: boolean): void {
        const request = this.cleanupRequest;
        if (!request?.inspection || request.loading || request.busy) return;
        this.cleanupRequest = {
            ...request,
            selectedObjectIds: selected ? request.inspection.candidates.map((candidate) => candidate.objectId) : [],
        };
    }

    async submitCleanup(): Promise<void> {
        const request = this.cleanupRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection || request.selectedObjectIds.length === 0 || request.loading || request.busy) return;
        if (sessionId === null) return;
        const selectedIds = request.selectedObjectIds.toSorted();
        const preferred = this.currentSourcePreference();
        const started = performance.now();
        this.cleanupRequest = { ...request, busy: true, error: '' };
        this.dependencies.setStatus(
            `Deleting ${selectedIds.length} unreferenced Wave Data ${selectedIds.length === 1 ? 'object' : 'objects'}`,
        );
        try {
            await this.dependencies.invalidateSession(sessionId);
            const rediscovered = await this.dependencies.transport.inspectWaveDataOrphans(sessionId, request.volumeId);
            if (
                waveDataCleanupFingerprint(rediscovered) !== waveDataCleanupFingerprint(request.inspection) ||
                selectedIds.some(
                    (objectId) => !rediscovered.candidates.some((candidate) => candidate.objectId === objectId),
                )
            ) {
                this.cleanupRequest = {
                    ...request,
                    inspection: rediscovered,
                    selectedObjectIds: rediscovered.candidates.map((candidate) => candidate.objectId),
                    busy: false,
                    error: 'The cleanup candidates changed. Review the current list before confirming again.',
                };
                this.dependencies.setStatus('Cleanup candidates changed; review before confirming again');
                return;
            }
            const deletion = await this.dependencies.transport.inspectObjectDeletion(sessionId, selectedIds, []);
            const targetImpacts = deletion.impacts.filter((impact) => impact.role === 'TARGET');
            const eligibleIds = deletion.selectedObjectIds.toSorted();
            if (
                !deletion.canApply ||
                targetImpacts.length !== selectedIds.length ||
                targetImpacts.some((impact) => impact.status !== 'REQUIRED' || !impact.selected) ||
                eligibleIds.join('\u0000') !== selectedIds.join('\u0000')
            ) {
                const current = await this.dependencies.transport.inspectWaveDataOrphans(sessionId, request.volumeId);
                this.cleanupRequest = {
                    ...request,
                    inspection: current,
                    selectedObjectIds: current.candidates.map((candidate) => candidate.objectId),
                    busy: false,
                    error: 'One or more Wave Data objects are no longer safe to delete. Review the current list.',
                };
                this.dependencies.setStatus('Wave Data cleanup requires another review');
                return;
            }
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startObjectDeletion(sessionId, selectedIds, []),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed')
                throw new Error(completed.error ?? 'Wave Data cleanup did not complete');
            await this.dependencies.refreshSession(preferred);
            this.removeDeletedFromSelection(selectedIds);
            ++this.cleanupGeneration;
            this.cleanupRequest = null;
            this.dependencies.reportTiming('cleanup-wave-data', started, selectedIds.length);
        } catch (error) {
            const message = userFacingMessage(error);
            this.dependencies.setStatus(message);
            ++this.cleanupGeneration;
            this.cleanupRequest = null;
            if (this.dependencies.sessionId() === sessionId) {
                await this.dependencies.refreshSession(preferred).catch(() => undefined);
            }
            this.dependencies.setStatus(`${message} The image has been refreshed.`);
        }
    }

    private async inspectObjects(generation = this.objectGeneration): Promise<void> {
        const request = this.objectRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) return;
        try {
            const inspection = await this.dependencies.transport.inspectObjectDeletion(
                sessionId,
                request.targets.map((target) => target.objectId),
                request.cleanupObjectIds,
            );
            if (
                generation !== this.objectGeneration ||
                deletionRequestKey(this.objectRequest?.targets ?? []) !== deletionRequestKey(request.targets) ||
                this.dependencies.sessionId() !== sessionId
            )
                return;
            this.objectRequest = { ...request, inspection, loading: false, error: '' };
        } catch (error) {
            if (
                generation !== this.objectGeneration ||
                deletionRequestKey(this.objectRequest?.targets ?? []) !== deletionRequestKey(request.targets)
            )
                return;
            this.objectRequest = {
                ...request,
                inspection: null,
                loading: false,
                error: userFacingMessage(error),
            };
        }
    }

    private async inspectCleanup(generation = this.cleanupGeneration): Promise<void> {
        const request = this.cleanupRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) return;
        try {
            const inspection = await this.dependencies.transport.inspectWaveDataOrphans(sessionId, request.volumeId);
            if (
                generation !== this.cleanupGeneration ||
                this.dependencies.sessionId() !== sessionId ||
                this.dependencies.activeVolumeId() !== request.volumeId ||
                this.cleanupRequest?.volumeId !== request.volumeId
            )
                return;
            this.cleanupRequest = {
                ...request,
                inspection,
                selectedObjectIds: inspection.candidates.map((candidate) => candidate.objectId),
                loading: false,
                error: '',
            };
        } catch (error) {
            if (
                generation !== this.cleanupGeneration ||
                this.dependencies.sessionId() !== sessionId ||
                this.cleanupRequest?.volumeId !== request.volumeId
            )
                return;
            this.cleanupRequest = {
                ...request,
                inspection: null,
                selectedObjectIds: [],
                loading: false,
                error: userFacingMessage(error),
            };
        }
    }

    private async recoverObjectFailure(
        error: unknown,
        request: ObjectDeletionRequest,
        sessionId: number,
        generation: number,
        preferred: { partitionIndex: number; volumeName?: string } | undefined,
    ): Promise<void> {
        const message = userFacingMessage(error);
        this.dependencies.setStatus(message);
        if (this.dependencies.sessionId() === sessionId) {
            await this.dependencies.refreshSession(preferred).catch(() => undefined);
        }
        if (
            generation !== this.objectGeneration ||
            deletionRequestKey(this.objectRequest?.targets ?? []) !== deletionRequestKey(request.targets) ||
            this.dependencies.sessionId() !== sessionId
        )
            return;
        this.objectRequest = {
            ...request,
            busy: false,
            loading: true,
            error: `${message} The image has been refreshed; review the deletion again.`,
        };
        const nextGeneration = ++this.objectGeneration;
        await this.inspectObjects(nextGeneration);
        if (
            nextGeneration === this.objectGeneration &&
            deletionRequestKey(this.objectRequest?.targets ?? []) === deletionRequestKey(request.targets)
        ) {
            this.objectRequest = {
                ...request,
                error: `${message} The image has been refreshed; review the deletion again.`,
            };
        }
    }

    private currentSourcePreference(): { partitionIndex: number; volumeName?: string } | undefined {
        const selectedSource = this.dependencies.selectedSource();
        return selectedSource.kind === 'volume' && selectedSource.partitionIndex !== undefined
            ? { partitionIndex: selectedSource.partitionIndex, volumeName: selectedSource.name }
            : undefined;
    }

    private removeDeletedFromSelection(objectIds: string[]): void {
        const deletedIds = new Set(objectIds);
        const selection = this.dependencies.selection();
        this.dependencies.setSelection({
            ...selection,
            items: selection.items.filter((item) => !deletedIds.has(item.objectId)),
        });
    }
}

function deletionRequestKey(targets: PackageExportObject[]): string {
    return targets
        .map((target) => target.objectId)
        .toSorted()
        .join('\u0000');
}

function deletionInspectionFingerprint(inspection: ObjectDeletionInspection): string {
    return JSON.stringify({
        selected: inspection.selectedObjectIds.toSorted(),
        targets: inspection.impacts
            .filter((impact) => impact.role === 'TARGET')
            .map((impact) => [impact.objectId, impact.status, impact.reason])
            .toSorted(([left], [right]) => left.localeCompare(right)),
    });
}

function waveDataCleanupFingerprint(inspection: WaveDataOrphanInspection): string {
    return JSON.stringify(
        inspection.candidates
            .map((candidate) => [
                candidate.objectId,
                candidate.storedSizeBytes,
                candidate.recoverableBytes,
                candidate.recoverableClusters,
            ])
            .toSorted(([left], [right]) => String(left).localeCompare(String(right))),
    );
}
