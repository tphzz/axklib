import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { ClientUploadLocation, FileLocation, ImageLocation, InputFileLocation } from '../../lib/storageLocations';
import type { AudioImportTarget, ImageTransport, OpenedImage, Tx16wImportMode } from '../../lib/transport';
import {
    requiredConversionIssues,
    type ConversionPlan,
    type ConversionFidelity,
    type ConversionSource,
} from '../../lib/conversions';
import type { PickerController } from '../dialogs/picker';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { tx16wDiskMediaType } from '../../lib/tx16wImport';
import type { JobController } from '../jobs/actions';
import { ImportCompletion } from './importCompletion.svelte';
import { AxklibApiError } from '../../lib/httpErrors';

export interface Tx16wVolumeOption {
    key: string;
    label: string;
    partitionName: string;
    target: AudioImportTarget;
}

export interface Tx16wImportMember {
    id: number;
    source: ClientUploadSource | FileLocation;
    sourceName: string;
    resolvedSource: InputFileLocation | null;
    upload: ClientUploadLocation | null;
}

export interface Tx16wImportRequest {
    members: Tx16wImportMember[];
    target: AudioImportTarget | null;
    importMode: Tx16wImportMode;
    plan: ConversionPlan | null;
    fidelity: ConversionFidelity;
    acknowledgedIssueIds: string[];
    documentSource?: Extract<ConversionSource, { kind: 'DOCUMENT' }>;
    destinationName?: string;
    status: 'waiting-target' | 'uploading' | 'inspecting' | 'ready' | 'importing';
    progress: number;
    error: string;
}

interface Tx16wImportDependencies {
    transport: ImageTransport;
    picker?: PickerController;
    jobs: JobController;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    mutationsAvailable: () => boolean;
    selectedSource: () => DiskTreeItem;
    sourceItems: () => DiskTreeItem[];
    refreshSession: (preferred: AudioImportTarget) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class Tx16wImportWorkflow {
    request = $state<Tx16wImportRequest | null>(null);
    private abortController: AbortController | null = null;
    private nextMemberId = 0;
    private ownedTarget: OpenedImage | null = null;
    private openingTarget = false;
    private targetGeneration = 0;
    private disposed = false;

    readonly completion: ImportCompletion;

    constructor(private readonly dependencies: Tx16wImportDependencies) {
        this.completion = new ImportCompletion(dependencies.transport, dependencies.jobs);
    }

    dropAvailable(): boolean {
        return this.dependencies.mutationsAvailable() && this.dependencies.imageLocation() !== null;
    }

    activeTarget(): AudioImportTarget | null {
        const selected = this.dependencies.selectedSource();
        return this.dropAvailable() && selected.kind === 'volume' && selected.partitionIndex !== undefined
            ? { partitionIndex: selected.partitionIndex, volumeName: selected.name }
            : null;
    }

    volumeOptions(): Tx16wVolumeOption[] {
        return collectTx16wVolumeOptions(this.ownedTarget?.tree ?? this.dependencies.sourceItems());
    }

    async requestNativeDocument(sessionId: number, documentId: string, expectedRevision: number): Promise<void> {
        if (!this.dependencies.picker || this.openingTarget || this.completion.locked || this.disposed) return;
        await this.close();
        if (this.request) return;
        this.openingTarget = true;
        const generation = ++this.targetGeneration;
        try {
            const location = await this.dependencies.picker.chooseLocation('file', 'Conversion destination', [
                'hds',
                'hda',
                'img',
            ]);
            if (!location || location.kind !== 'server-file' || generation !== this.targetGeneration || this.disposed)
                return;
            this.dependencies.setStatus('Opening conversion destination');
            let opened = await this.dependencies.transport.openImage([location]);
            if (generation !== this.targetGeneration || this.disposed) {
                await this.dependencies.transport.closeImage(opened.sessionId);
                return;
            }
            this.ownedTarget = opened;
            const documents = opened.documents.filter((document) => document.formatId === 'yamaha.a-series');
            if (documents.length !== 1) throw new Error('Choose an image with one A-series document');
            if (opened.documentId !== documents[0].id)
                opened = await this.dependencies.transport.selectDocument(opened.sessionId, documents[0].id);
            if (generation !== this.targetGeneration || this.disposed) return;
            this.ownedTarget = opened;
            if (!opened.volumeMutationsAvailable) throw new Error('Conversion requires a writable A-series SFS image');
            this.completion.reset();
            this.request = {
                members: [],
                target: null,
                importMode: 'HIERARCHY',
                plan: null,
                fidelity: 'BEST',
                acknowledgedIssueIds: [],
                status: 'waiting-target',
                progress: 0,
                error: '',
                documentSource: { kind: 'DOCUMENT', sessionId, documentId, expectedRevision },
                destinationName: location.displayName,
            };
        } catch (error) {
            this.dependencies.setStatus(userFacingMessage(error));
            await this.releaseAndClose();
        } finally {
            this.openingTarget = false;
        }
    }

    async requestDroppedFiles(
        files: (ClientUploadSource | FileLocation)[],
        target: AudioImportTarget | null = this.activeTarget(),
    ): Promise<void> {
        if (this.openingTarget || this.disposed || this.completion.locked) return;
        const admitted = admittedSources(files);
        if (admitted.length !== files.length || admitted.length === 0) {
            this.dependencies.setStatus('Drop one or more TX16W .img or .ima disk images');
            return;
        }
        if (admitted.length > 32) {
            this.dependencies.setStatus('A TX16W disk set can contain at most 32 images');
            return;
        }
        if (!this.dropAvailable()) {
            this.dependencies.setStatus('TX16W import requires a writable SFS hard-disk image');
            return;
        }
        await this.close();
        if (this.request) return;
        this.completion.reset();
        this.request = {
            members: admitted.map((source) => this.member(source)),
            target,
            importMode: 'HIERARCHY',
            plan: null,
            fidelity: 'BEST',
            acknowledgedIssueIds: [],
            status: target ? 'inspecting' : 'waiting-target',
            progress: 0,
            error: '',
        };
        if (target) await this.inspect();
    }

    async addFiles(files: (ClientUploadSource | FileLocation)[]): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked || request.documentSource) return;
        const admitted = admittedSources(files);
        if (admitted.length !== files.length || admitted.length === 0) {
            request.error = 'Choose one or more TX16W .img or .ima disk images';
            return;
        }
        if (request.members.length + admitted.length > 32) {
            request.error = 'A TX16W disk set can contain at most 32 images';
            return;
        }
        request.members.push(...admitted.map((source) => this.member(source)));
        request.error = '';
        if (request.target) await this.inspect();
    }

    async removeMember(memberId: number): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked || request.documentSource) return;
        this.abortController?.abort();
        const index = request.members.findIndex((member) => member.id === memberId);
        if (index < 0) return;
        const removed = request.members[index];
        if (removed.upload) {
            try {
                await this.dependencies.transport.releaseClientUpload(removed.upload);
            } catch (error) {
                request.error = userFacingMessage(error);
                return;
            }
        }
        request.members.splice(index, 1);
        request.error = '';
        if (request.members.length === 0) {
            await this.close();
        } else if (request.target) {
            await this.inspect();
        }
    }

    async selectTarget(target: AudioImportTarget | null): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked) return;
        request.target = target;
        request.error = '';
        if (target) await this.inspect();
        else {
            this.abortController?.abort();
            await this.discardPlan(request);
            request.status = 'waiting-target';
        }
    }

    async selectImportMode(importMode: Tx16wImportMode): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked || request.importMode === importMode) return;
        request.importMode = importMode;
        request.error = '';
        if (request.target) await this.inspect();
    }

    async commit(): Promise<void> {
        if (this.completion.locked) return;
        const request = this.request;
        const sessionId = this.ownedTarget?.sessionId ?? this.dependencies.sessionId();
        if (
            !request?.plan?.report?.executable ||
            requiredConversionIssues(request.plan).some((id) => !request.acknowledgedIssueIds.includes(id)) ||
            !request.target ||
            sessionId === null
        ) {
            return;
        }
        const target = request.target;
        const plan = request.plan;
        const objectCount = plan.report!.actionCount;
        const started = performance.now();
        request.status = 'importing';
        request.error = '';
        this.dependencies.setStatus('Importing TX16W disk set');
        try {
            if (!this.ownedTarget) await this.dependencies.invalidateSession(sessionId);
            await this.completion.run(
                () => this.dependencies.transport.executeConversion(plan, request.acknowledgedIssueIds),
                async () => {
                    if (this.ownedTarget) this.ownedTarget = await this.dependencies.transport.refreshImage(sessionId);
                    else {
                        this.dependencies.selectWorkspace('programs');
                        await this.dependencies.refreshSession(target);
                    }
                    this.dependencies.reportTiming('tx16w-disk-set-import', started, objectCount);
                    this.dependencies.setStatus(
                        request.documentSource
                            ? `Converted TX16W document to ${request.destinationName}`
                            : `Imported ${request.members.length} TX16W disk image${request.members.length === 1 ? '' : 's'}`,
                    );
                    if (this.completion.warnings.length === 0) await this.releaseAndClose();
                },
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
                plan.report!.issues.map((issue) => issue.message),
                async () => this.dependencies.transport.conversionExecution(plan.planId),
            );
            this.updateCompletionError();
            if (this.completion.failure) request.plan = null;
        } catch (error) {
            const message = userFacingMessage(error);
            request.status = 'ready';
            request.error = message;
            this.dependencies.setStatus(message);
        }
    }

    async recoverCompletion(): Promise<void> {
        await this.completion.recover();
        this.updateCompletionError();
        if (this.completion.failure && this.request) this.request.plan = null;
    }

    async selectFidelity(fidelity: ConversionFidelity): Promise<void> {
        if (!this.request || this.completion.locked) return;
        this.request.fidelity = fidelity;
        await this.inspect();
    }

    async reprepare(): Promise<void> {
        if (!this.completion.locked) await this.inspect();
    }

    private async discardPlan(request: Tx16wImportRequest): Promise<void> {
        const plan = request.plan;
        if (plan?.state === 'READY') {
            try {
                await this.dependencies.transport.releaseConversion(plan.planId);
            } catch (error) {
                if (!(error instanceof AxklibApiError && error.status === 404)) throw error;
            }
        }
        if (request.plan === plan) {
            request.plan = null;
            request.acknowledgedIssueIds = [];
        }
    }

    private updateCompletionError(): void {
        if (!this.request) return;
        if (!this.completion.locked) this.request.status = 'ready';
        this.request.error = this.completion.message;
        this.dependencies.setStatus(this.completion.message);
    }

    async close(): Promise<void> {
        if (this.completion.busy || this.completion.phase === 'unconfirmed') return;
        ++this.targetGeneration;
        try {
            await this.releaseAndClose();
        } catch (error) {
            if (this.request) this.request.error = userFacingMessage(error);
            this.dependencies.setStatus(userFacingMessage(error));
        }
    }

    async dispose(): Promise<void> {
        this.disposed = true;
        ++this.targetGeneration;
        await this.close();
    }

    private async releaseAndClose(): Promise<void> {
        this.abortController?.abort();
        this.abortController = null;
        const request = this.request;
        if (request?.plan?.state === 'READY' && !this.completion.locked) await this.discardPlan(request);
        for (const member of request?.members ?? []) {
            if (!member.upload) continue;
            await this.dependencies.transport.releaseClientUpload(member.upload);
            member.upload = null;
            member.resolvedSource = null;
        }
        if (this.ownedTarget) {
            await this.dependencies.transport.closeImage(this.ownedTarget.sessionId);
            this.ownedTarget = null;
        }
        if (this.request === request) this.request = null;
    }

    private member(source: ClientUploadSource | FileLocation): Tx16wImportMember {
        return {
            id: this.nextMemberId++,
            source,
            sourceName: sourceName(source),
            resolvedSource: isFileLocation(source) ? source : null,
            upload: null,
        };
    }

    private async inspect(): Promise<void> {
        const request = this.request;
        const sessionId = this.ownedTarget?.sessionId ?? this.dependencies.sessionId();
        if (!request?.target || (!request.documentSource && request.members.length === 0) || sessionId === null) return;
        this.abortController?.abort();
        const controller = new AbortController();
        this.abortController = controller;
        request.error = '';
        request.status = 'inspecting';
        try {
            await this.discardPlan(request);
            for (const [index, member] of request.members.entries()) {
                if (member.resolvedSource) continue;
                request.status = 'uploading';
                const upload = await this.dependencies.transport.uploadClientFile(
                    member.source as ClientUploadSource,
                    'DISK_IMAGE',
                    (sent, total) => {
                        if (this.request === request) {
                            request.progress = (index + (total === 0 ? 0 : sent / total)) / request.members.length;
                        }
                    },
                    controller.signal,
                );
                if (controller.signal.aborted || this.request !== request) {
                    await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
                    return;
                }
                member.upload = upload;
                member.resolvedSource = upload;
            }
            if (controller.signal.aborted || this.request !== request) return;
            request.status = 'inspecting';
            request.progress = 1;
            const plan = await this.dependencies.transport.prepareConversion(
                {
                    profileId: 'yamaha.tx16w-to-a-series',
                    source: request.documentSource ?? {
                        kind: 'SOURCES',
                        sources: request.members.map((member) => member.resolvedSource as InputFileLocation),
                    },
                    destinationSessionId: sessionId,
                    selection: { ...request.target },
                    options: { mode: request.importMode },
                    fidelity: request.fidelity,
                    capacityPolicy: this.completion.capacity.policy(),
                },
                controller.signal,
            );
            if (controller.signal.aborted || this.request !== request) {
                await this.dependencies.transport.releaseConversion(plan.planId);
                return;
            }
            request.plan = plan;
            request.status = 'ready';
        } catch (error) {
            if (controller.signal.aborted || this.request !== request) return;
            request.status = request.target ? 'ready' : 'waiting-target';
            request.error = userFacingMessage(error);
            this.dependencies.setStatus(request.error);
        } finally {
            if (this.abortController === controller) this.abortController = null;
        }
    }
}

export function collectTx16wVolumeOptions(items: readonly DiskTreeItem[]): Tx16wVolumeOption[] {
    const result: Tx16wVolumeOption[] = [];
    function visit(nodes: readonly DiskTreeItem[], partitionName = ''): void {
        for (const item of nodes) {
            const nextPartitionName = item.kind === 'partition' ? item.name : partitionName;
            if (item.kind === 'volume' && item.partitionIndex !== undefined) {
                const target = { partitionIndex: item.partitionIndex, volumeName: item.name };
                result.push({
                    key: `${target.partitionIndex}:${target.volumeName}`,
                    label: `${nextPartitionName || `Partition ${target.partitionIndex + 1}`} · ${item.name}`,
                    partitionName: nextPartitionName || `Partition ${target.partitionIndex + 1}`,
                    target,
                });
            }
            visit(item.children ?? [], nextPartitionName);
        }
    }
    visit(items);
    return result;
}

function admittedSources(files: (ClientUploadSource | FileLocation)[]): (ClientUploadSource | FileLocation)[] {
    return files.filter((source) => tx16wDiskMediaType(sourceName(source)) !== null);
}

function isFileLocation(source: ClientUploadSource | FileLocation): source is FileLocation {
    return 'kind' in source && source.kind === 'server-file';
}

function sourceName(source: ClientUploadSource | FileLocation): string {
    return isFileLocation(source)
        ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName)
        : source.name;
}
