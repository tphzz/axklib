import { nativeFileSource } from '../../lib/nativeFileSource';
import { selectLocalPackage } from '../../lib/nativePackages';
import type { ClientUploadLocation, InputFileLocation } from '../../lib/storageLocations';
import type {
    ImageSessionPackageImportPlan,
    ImageTransport,
    PackageInspection,
    PackageOpaqueSequenceDecision,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { reportError } from '../../lib/diagnostics';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackagePickerHistory } from './packagePickerHistory';

const packageExtensions = ['axkvol', 'axkprg', 'axksbac', 'axksbnk', 'axksmpl', 'axkseq', 'axkpkg'];
const packageExtensionSet = new Set(packageExtensions);

export interface PackageImportRequest {
    item: DiskTreeItem;
    source: InputFileLocation | null;
    upload: ClientUploadLocation | null;
    localSourcePath: string | null;
    sourceName: string;
    inspection: PackageInspection | null;
    plan: ImageSessionPackageImportPlan | null;
    renames: Record<string, string>;
    programSlots: Record<string, number>;
    opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
    hasUnvalidatedChanges: boolean;
    status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
    progress: number;
    error: string;
}

interface PackageImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    invalidateSession: (sessionId: number) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
    pickerHistory?: PackagePickerHistory;
}

export class PackageImportWorkflow {
    request = $state<PackageImportRequest | null>(null);
    private generation = 0;
    private abortController: AbortController | null = null;
    private readonly pickerHistory: PackagePickerHistory;

    constructor(private readonly dependencies: PackageImportDependencies) {
        this.pickerHistory = dependencies.pickerHistory ?? new PackagePickerHistory();
    }

    open(item: DiskTreeItem): void {
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        this.request = {
            item,
            source: null,
            upload: null,
            localSourcePath: null,
            sourceName: '',
            inspection: null,
            plan: null,
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'choosing',
            progress: 0,
            error: '',
        };
    }

    rename(nodeId: string, name: string): void {
        if (!this.request || this.request.status !== 'ready' || this.request.renames[nodeId] === name) return;
        this.request = {
            ...this.request,
            renames: { ...this.request.renames, [nodeId]: name },
            hasUnvalidatedChanges: true,
        };
    }

    programSlot(nodeId: string, slot: number): void {
        if (
            !this.request ||
            this.request.status !== 'ready' ||
            !Number.isInteger(slot) ||
            slot < 1 ||
            slot > 128 ||
            this.request.programSlots[nodeId] === slot
        ) {
            return;
        }
        this.request = {
            ...this.request,
            programSlots: { ...this.request.programSlots, [nodeId]: slot },
            hasUnvalidatedChanges: true,
        };
    }

    programStart(placementId: string, start: number): void {
        if (!this.request?.plan || this.request.status !== 'ready' || !Number.isInteger(start)) return;
        const placement = this.request.plan.programSlotPlacements.find((entry) => entry.placementId === placementId);
        if (!placement || placement.mode !== 'CONTIGUOUS' || start < 1 || start + placement.mappings.length - 1 > 128) {
            return;
        }
        const programSlots = { ...this.request.programSlots };
        let changed = false;
        placement.mappings.forEach((mapping, index) => {
            const destinationSlot = start + index;
            changed = changed || programSlots[mapping.nodeId] !== destinationSlot;
            programSlots[mapping.nodeId] = destinationSlot;
        });
        if (!changed) return;
        this.request = { ...this.request, programSlots, hasUnvalidatedChanges: true };
    }

    opaqueSequenceAction(nodeId: string, action: PackageOpaqueSequenceDecision['action']): void {
        if (!this.request || this.request.status !== 'ready' || this.request.opaqueSequenceActions[nodeId] === action) {
            return;
        }
        this.request = {
            ...this.request,
            opaqueSequenceActions: { ...this.request.opaqueSequenceActions, [nodeId]: action },
            hasUnvalidatedChanges: true,
        };
        void this.replan();
    }

    async dispose(): Promise<void> {
        const request = this.request;
        this.request = null;
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        if (request) await this.releaseResources(request);
    }

    async close(): Promise<void> {
        if (this.request?.status === 'applying') return;
        await this.dispose();
    }

    async resetSource(): Promise<void> {
        if (!this.request || this.request.status === 'applying') return;
        const request = this.request;
        ++this.generation;
        this.abortController?.abort();
        this.abortController = null;
        await this.releaseResources(request);
        this.request = {
            ...request,
            source: null,
            upload: null,
            localSourcePath: null,
            sourceName: '',
            inspection: null,
            plan: null,
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'choosing',
            progress: 0,
            error: '',
        };
    }

    async chooseWorkspace(): Promise<void> {
        if (!this.request) return;
        const selection = await this.dependencies.picker.chooseLocation(
            'file',
            'Choose axklib package',
            packageExtensions,
            '',
            {
                parentDialog: 'package-import',
                initialDirectory: this.pickerHistory.lastDirectory,
                initialFile: this.pickerHistory.lastImportedWorkspaceFile,
                ondirectorychange: (directory) => (this.pickerHistory.lastDirectory = directory),
            },
        );
        if (selection?.kind !== 'server-file' || !this.request) return;
        await this.inspect(selection, selection.displayName);
    }

    async chooseLocal(): Promise<void> {
        if (!this.request || !this.dependencies.isDesktop) return;
        let controller: AbortController | null = null;
        let generation = -1;
        try {
            const path = await selectLocalPackage(this.pickerHistory.lastImportedLocalPath);
            if (!path || !this.request) return;
            const file = await nativeFileSource(path, packageExtensionSet, 'application/octet-stream');
            controller = new AbortController();
            this.abortController?.abort();
            this.abortController = controller;
            generation = ++this.generation;
            this.request = {
                ...this.request,
                sourceName: file.name,
                source: null,
                upload: null,
                localSourcePath: path,
                inspection: null,
                plan: null,
                renames: {},
                programSlots: {},
                opaqueSequenceActions: {},
                hasUnvalidatedChanges: false,
                status: 'loading',
                progress: 0,
                error: '',
            };
            const upload = await this.dependencies.transport.uploadClientFile(
                file,
                'PACKAGE',
                (sent, total) => {
                    if (generation === this.generation && this.request) {
                        this.request = { ...this.request, progress: total === 0 ? 0 : sent / total };
                    }
                },
                controller.signal,
            );
            if (this.abortController === controller) this.abortController = null;
            if (generation !== this.generation || !this.request) {
                await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            await this.inspect(upload, file.name, upload, path);
        } catch (error) {
            if (this.abortController === controller) this.abortController = null;
            if (generation >= 0 && (generation !== this.generation || !this.request)) return;
            reportError('Import local package failed', error);
            if (this.request) {
                this.request = { ...this.request, status: 'choosing', error: userFacingMessage(error) };
            }
        }
    }

    async replan(): Promise<void> {
        if (!this.request?.source || this.request.status === 'applying') return;
        const previousPlan = this.request.plan;
        this.request = { ...this.request, status: 'planning', error: '' };
        const generation = ++this.generation;
        try {
            await this.plan(generation, previousPlan?.planToken);
        } catch (error) {
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: userFacingMessage(error) };
            }
        }
    }

    async apply(): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (
            !request?.source ||
            !request.plan?.valid ||
            request.hasUnvalidatedChanges ||
            sessionId === null ||
            request.item.partitionIndex === undefined
        ) {
            return;
        }
        const importedSource = request.source;
        const generation = ++this.generation;
        this.request = { ...request, status: 'applying', error: '' };
        this.dependencies.setStatus(`Importing package into ${request.item.name}`);
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startImagePackageImport(request.plan!.planToken),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Package import did not complete');
            if (importedSource.kind === 'server-file') {
                this.pickerHistory.lastImportedWorkspaceFile = importedSource.reference;
            } else if (request.localSourcePath) {
                this.pickerHistory.lastImportedLocalPath = request.localSourcePath;
            }
            if (request.upload) {
                await this.dependencies.transport.releaseClientUpload(request.upload).catch(() => undefined);
            }
            this.request = null;
            await this.dependencies.refreshSession({
                partitionIndex: request.item.partitionIndex,
                volumeName: request.item.name,
            });
            this.dependencies.setStatus(`Imported package into ${request.item.name}`);
        } catch (error) {
            const message = userFacingMessage(error);
            this.dependencies.setStatus(message);
            if (generation === this.generation && this.request) {
                this.request = { ...this.request, status: 'ready', error: message };
            }
        }
    }

    private async releaseResources(request: PackageImportRequest): Promise<void> {
        if (request.plan) {
            await this.dependencies.transport
                .releaseImagePackageImportPlan(request.plan.planToken)
                .catch(() => undefined);
        }
        if (request.upload) {
            await this.dependencies.transport.releaseClientUpload(request.upload).catch(() => undefined);
        }
    }

    private async inspect(
        source: InputFileLocation,
        sourceName: string,
        upload: ClientUploadLocation | null = null,
        localSourcePath: string | null = null,
    ): Promise<void> {
        if (!this.request) return;
        const generation = ++this.generation;
        this.request = {
            ...this.request,
            source,
            upload,
            localSourcePath,
            sourceName,
            inspection: null,
            plan: null,
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
            hasUnvalidatedChanges: false,
            status: 'loading',
            progress: 0,
            error: '',
        };
        try {
            const inspection = await this.dependencies.transport.inspectPackage(source, false);
            if (generation !== this.generation || !this.request) {
                if (upload) await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            this.request = { ...this.request, inspection, status: 'planning' };
            await this.plan(generation);
        } catch (error) {
            if (generation !== this.generation || !this.request) {
                if (upload) await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
                return;
            }
            this.request = { ...this.request, status: 'choosing', error: userFacingMessage(error) };
        }
    }

    private async plan(generation: number, replacePlanToken?: string): Promise<void> {
        const automaticPlanToken = await this.planOnce(generation, replacePlanToken, true);
        if (!automaticPlanToken) return;
        try {
            await this.planOnce(generation, automaticPlanToken, false);
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
        if (!request?.source || sessionId === null || request.item.partitionIndex === undefined) return null;
        if (generation !== this.generation) return null;
        this.request = {
            ...request,
            status: 'planning',
            plan: replacePlanToken ? request.plan : null,
            error: '',
        };
        const renames = Object.entries(request.renames)
            .map(([nodeId, destinationName]) => ({ packageIndex: 0, nodeId, destinationName: destinationName.trim() }))
            .filter((rename) => rename.destinationName.length > 0);
        const programSlotAssignments = Object.entries(request.programSlots)
            .map(([nodeId, destinationSlot]) => ({ packageIndex: 0, nodeId, destinationSlot }))
            .sort(
                (left, right) =>
                    left.destinationSlot - right.destinationSlot || left.nodeId.localeCompare(right.nodeId),
            );
        const opaqueSequenceDecisions = Object.entries(request.opaqueSequenceActions)
            .map(([nodeId, action]) => ({ packageIndex: 0, nodeId, action }))
            .sort((left, right) => left.nodeId.localeCompare(right.nodeId));
        const plan = await this.dependencies.transport.planImagePackageImport(
            sessionId,
            [request.source],
            {
                kind: 'EXISTING_VOLUME',
                partitionIndex: request.item.partitionIndex,
                volumeName: request.item.name,
            },
            renames,
            programSlotAssignments,
            replacePlanToken,
            opaqueSequenceDecisions,
        );
        if (generation !== this.generation || !this.request) {
            await this.dependencies.transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
            return null;
        }
        const placementNodeIds = new Set(
            plan.programSlotPlacements.flatMap((placement) => placement.mappings.map((mapping) => mapping.nodeId)),
        );
        const nextRenames = Object.fromEntries(
            Object.entries(request.renames).filter(([nodeId]) => !placementNodeIds.has(nodeId)),
        );
        for (const action of plan.actions) {
            if (
                !placementNodeIds.has(action.nodeId) &&
                plan.conflicts.some((conflict) => conflict.nodeId === action.nodeId) &&
                !nextRenames[action.nodeId]
            ) {
                nextRenames[action.nodeId] = action.destinationName;
            }
        }
        const nextProgramSlots = { ...request.programSlots };
        let addedSuggestedProgramSlots = false;
        for (const placement of plan.programSlotPlacements) {
            for (const mapping of placement.mappings) {
                if (nextProgramSlots[mapping.nodeId] === undefined) {
                    nextProgramSlots[mapping.nodeId] = mapping.destinationSlot;
                    addedSuggestedProgramSlots = true;
                }
            }
        }
        const checkSuggestedProgramSlots =
            allowAutomaticProgramSlotCheck &&
            addedSuggestedProgramSlots &&
            plan.programSlotPlacements.some(
                (placement) => !placement.applied && placement.mode !== 'UNAVAILABLE' && placement.mappings.length > 0,
            );
        this.request = {
            ...this.request,
            plan,
            renames: nextRenames,
            programSlots: nextProgramSlots,
            hasUnvalidatedChanges: addedSuggestedProgramSlots,
            status: checkSuggestedProgramSlots ? 'planning' : 'ready',
            error: '',
        };
        return checkSuggestedProgramSlots ? plan.planToken : null;
    }
}
