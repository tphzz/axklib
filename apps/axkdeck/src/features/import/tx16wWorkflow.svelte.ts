import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { ClientUploadLocation, FileLocation, ImageLocation, InputFileLocation } from '../../lib/storageLocations';
import type {
    AudioImportTarget,
    ImageTransport,
    OpenedImage,
    Tx16wImportMode,
    VolumeImportDestination,
} from '../../lib/transport';
import { collectImportDestinations } from './packageDestinations';
import { droppedPlacement, placementError, type PlacementOptions } from './tx16wPlacement';
import {
    requiredConversionIssues,
    type ConversionPlan,
    type ConversionFidelity,
    type ConversionSource,
    type ConversionSourceObject,
} from '../../lib/conversions';
import type { PickerController } from '../dialogs/picker';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { tx16wDiskMediaType } from '../../lib/tx16wImport';
import type { JobController } from '../jobs/actions';
import { ImportCompletion } from './importCompletion.svelte';
import { AxklibApiError } from '../../lib/httpErrors';
import { imageLocation } from '../../lib/httpImageSources';
import {
    idleCompanionSearch,
    Tx16wCompanionDiscovery,
    tx16wSourceKey,
    type Tx16wCompanionSearch,
} from './tx16wCompanions';

export interface Tx16wImportMember {
    id: number;
    source: ClientUploadSource | FileLocation;
    sourceName: string;
    resolvedSource: InputFileLocation | null;
    upload: ClientUploadLocation | null;
    origin: 'SELECTED' | 'AUTO_ADDED';
    sourceIdentity: string | object;
}

export interface Tx16wImportRequest {
    sourceFormat?: 'yamaha.tx16w' | 'nuedge.typhoon';
    members: Tx16wImportMember[];
    target: VolumeImportDestination | null;
    importMode: Tx16wImportMode;
    plan: ConversionPlan | null;
    fidelity: ConversionFidelity;
    acknowledgedIssueIds: string[];
    documentSource?: Extract<ConversionSource, { kind: 'DOCUMENT' }>;
    destinationProfile?: string;
    destinationName?: string;
    sourceName?: string;
    sourceObjects?: ConversionSourceObject[];
    sourceObjectIds?: string[];
    discovery: Tx16wCompanionSearch;
    status: 'waiting-target' | 'opening-target' | 'uploading' | 'discovering' | 'inspecting' | 'ready' | 'importing';
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
    private ownedTarget = $state.raw<OpenedImage | null>(null);
    private openingTarget = false;
    private targetGeneration = 0;
    private disposed = false;
    private readonly companions: Tx16wCompanionDiscovery;
    private readonly releasingMembers = new WeakMap<Tx16wImportMember, Promise<void>>();
    private readonly unreleasedPlans = new Set<string>();

    readonly completion: ImportCompletion;

    constructor(private readonly dependencies: Tx16wImportDependencies) {
        this.completion = new ImportCompletion(dependencies.transport, dependencies.jobs);
        this.companions = new Tx16wCompanionDiscovery(
            dependencies.transport,
            (source) => this.member(source),
            (message) => dependencies.setStatus(message),
        );
    }

    dropAvailable(): boolean {
        return this.dependencies.mutationsAvailable() && this.dependencies.imageLocation() !== null;
    }

    destinations(): PlacementOptions {
        return collectImportDestinations(
            this.ownedTarget?.tree ?? (this.request?.documentSource ? [] : this.dependencies.sourceItems()),
        );
    }

    private canStart(): boolean {
        return (
            !this.disposed &&
            !this.openingTarget &&
            (!this.completion.locked || (!this.request && this.completion.canDismiss))
        );
    }

    private async loadDestinationProfile(request: Tx16wImportRequest): Promise<void> {
        const profiles = await this.dependencies.transport.conversionProfiles();
        if (this.request !== request) return;
        const profile = profiles.find(
            (p) =>
                p.sourceFormat === request.sourceFormat &&
                p.destinationFormat === 'yamaha.a-series' &&
                p.outputKind === 'EXISTING_IMAGE',
        );
        if (!profile) throw new Error('This server does not offer conversion to an existing A-series image');
        request.destinationProfile = profile.id;
    }

    async requestNativeDocument(
        sessionId: number,
        documentId: string,
        expectedRevision: number,
        session?: Pick<OpenedImage, 'sources' | 'documents'>,
    ): Promise<void> {
        if (!this.dependencies.picker || !this.canStart()) return;
        await this.close();
        if (this.request) return;
        this.completion.reset();
        const document = session?.documents.find((d) => d.id === documentId);
        if (session && document?.formatId !== 'yamaha.tx16w' && document?.formatId !== 'nuedge.typhoon') return;
        const members = document?.sourceMemberIds;
        const sourceName = session?.sources
            .filter((s) => members?.includes(s.id))
            .map((s) => imageLocation(s.source).displayName.split('/').at(-1))
            .join(' + ');
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
            sourceFormat: document?.formatId === 'nuedge.typhoon' ? 'nuedge.typhoon' : 'yamaha.tx16w',
            sourceName: sourceName || this.dependencies.imageLocation()?.displayName,
            discovery: idleCompanionSearch(),
        };
        const request = this.request;
        request.status = 'inspecting';
        try {
            await this.loadDestinationProfile(request);
        } catch (error) {
            if (this.request === request) request.error = userFacingMessage(error);
        } finally {
            if (this.request === request) request.status = 'waiting-target';
        }
    }

    async browseNativeDestination(): Promise<void> {
        const request = this.request;
        if (
            !request?.documentSource ||
            !this.dependencies.picker ||
            !['waiting-target', 'ready'].includes(request.status) ||
            this.openingTarget ||
            this.completion.locked ||
            this.disposed
        )
            return;
        this.openingTarget = true;
        const priorStatus = request.status;
        request.status = 'opening-target';
        const generation = ++this.targetGeneration;
        let candidate: OpenedImage | null = null;
        try {
            if (!request.destinationProfile) {
                await this.loadDestinationProfile(request);
                if (generation !== this.targetGeneration || this.disposed) return;
            }
            const location = await this.dependencies.picker.chooseLocation(
                'file',
                'Choose existing A-series image',
                ['hds', 'hda', 'img'],
                '',
                { parentDialog: 'conversion-destination' },
            );
            if (generation !== this.targetGeneration || this.disposed) return;
            if (!location || location.kind !== 'server-file') {
                request.status = priorStatus;
                return;
            }
            this.abortController?.abort();
            request.error = '';
            candidate = await this.dependencies.transport.openImage([location]);
            if (generation !== this.targetGeneration || this.disposed) return;
            const documents = candidate.documents.filter((document) => document.formatId === 'yamaha.a-series');
            if (documents.length !== 1) throw new Error('Choose an image with one A-series document');
            if (candidate.documentId !== documents[0].id)
                candidate = await this.dependencies.transport.selectDocument(candidate.sessionId, documents[0].id);
            if (generation !== this.targetGeneration || this.disposed) return;
            if (!candidate.volumeMutationsAvailable)
                throw new Error('Conversion requires a writable A-series SFS image');
            await this.discardPlan(request);
            if (generation !== this.targetGeneration || this.disposed) return;
            const previous = this.ownedTarget;
            this.ownedTarget = candidate;
            candidate = null;
            request.target = null;
            request.destinationName = location.displayName;
            request.status = 'waiting-target';
            if (previous) await this.dependencies.transport.closeImage(previous.sessionId);
        } catch (error) {
            if (this.request === request && generation === this.targetGeneration) {
                request.error = userFacingMessage(error);
                request.status = request.target ? 'ready' : 'waiting-target';
                this.dependencies.setStatus(request.error);
            }
        } finally {
            this.openingTarget = false;
            if (candidate) await this.dependencies.transport.closeImage(candidate.sessionId);
        }
    }

    async requestDroppedFiles(
        files: (ClientUploadSource | FileLocation)[],
        selected: DiskTreeItem | null = this.dependencies.selectedSource(),
        claimUpload?: (source: ClientUploadSource | FileLocation) => ClientUploadLocation | undefined,
        sourceFormat: NonNullable<Tx16wImportRequest['sourceFormat']> = 'yamaha.tx16w',
    ): Promise<void> {
        if (!this.canStart()) return;
        const admitted = admittedSources(files);
        if (files.some((source) => tx16wDiskMediaType(sourceName(source)) === null) || admitted.length === 0) {
            this.dependencies.setStatus('Drop one or more TX16W .img or .ima disk images');
            return;
        }
        if (admitted.length > 32) {
            this.dependencies.setStatus('A TX16W disk set can contain at most 32 images');
            return;
        }
        if (
            admitted.some(
                (source) =>
                    !isFileLocation(source) &&
                    (!Number.isSafeInteger(source.size) || source.size < 0 || source.size > 4 * 1024 * 1024),
            )
        ) {
            this.dependencies.setStatus('Each TX16W disk image must be at most 4 MiB');
            return;
        }
        if (!this.dropAvailable()) {
            this.dependencies.setStatus('TX16W import requires a writable SFS hard-disk image');
            return;
        }
        await this.close();
        if (this.request) return;
        this.completion.reset();
        const target = droppedPlacement(selected, sourceName(admitted[0]));
        this.request = {
            members: admitted.map((source) => {
                const member = this.member(source);
                const upload = claimUpload?.(source);
                if (upload) member.upload = member.resolvedSource = upload;
                return member;
            }),
            sourceFormat,
            target,
            importMode: 'HIERARCHY',
            plan: null,
            fidelity: 'BEST',
            acknowledgedIssueIds: [],
            discovery: idleCompanionSearch(),
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
        if (files.some((source) => tx16wDiskMediaType(sourceName(source)) === null) || admitted.length === 0) {
            request.error = 'Choose one or more TX16W .img or .ima disk images';
            return;
        }
        if (
            admitted.some(
                (source) =>
                    !isFileLocation(source) &&
                    (!Number.isSafeInteger(source.size) || source.size < 0 || source.size > 4 * 1024 * 1024),
            )
        ) {
            request.error = 'Each TX16W disk image must be at most 4 MiB';
            return;
        }
        const existing = new Map(request.members.map((member) => [member.sourceIdentity, member]));
        const additions = admitted.filter((source) => !existing.has(tx16wSourceKey(source)));
        if (request.members.length + additions.length > 32) {
            request.error = 'A TX16W disk set can contain at most 32 images';
            return;
        }
        for (const source of admitted) {
            this.companions.include(source);
            const old = existing.get(tx16wSourceKey(source));
            if (old) old.origin = 'SELECTED';
        }
        request.members.push(...additions.map((source) => this.member(source)));
        if (additions.length) this.resetEligibility(request);
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
        const generation = this.targetGeneration;
        request.status = 'inspecting';
        request.error = '';
        try {
            await this.discardPlan(request);
            if (this.request !== request || generation !== this.targetGeneration) return;
            await this.releaseMemberUpload(removed);
            if (this.request !== request || generation !== this.targetGeneration) return;
            this.companions.exclude(removed);
            request.members.splice(index, 1);
            this.resetEligibility(request);
            if (request.members.length === 0) {
                request.discovery = idleCompanionSearch();
                request.status = request.target ? 'ready' : 'waiting-target';
            } else if (request.target) {
                await this.inspect();
            } else request.status = 'waiting-target';
        } catch (error) {
            if (this.request !== request || generation !== this.targetGeneration) return;
            request.status = request.target ? 'ready' : 'waiting-target';
            request.error = userFacingMessage(error);
        }
    }

    async selectTarget(target: VolumeImportDestination | null, typing = false): Promise<void> {
        const request = this.request;
        if (
            !request ||
            this.openingTarget ||
            this.completion.locked ||
            request.status === 'importing' ||
            (request.documentSource && !this.ownedTarget)
        )
            return;
        request.target = target;
        request.error = '';
        await this.inspect(false, typing);
    }

    async selectImportMode(importMode: Tx16wImportMode): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked || request.importMode === importMode) return;
        request.importMode = importMode;
        this.resetEligibility(request);
        request.error = '';
        if (request.target) await this.inspect();
    }

    private resetEligibility(request: Tx16wImportRequest) {
        request.sourceObjects = undefined;
        if (request.sourceObjectIds !== undefined) request.sourceObjectIds = [];
    }

    async selectSources(ids: string[] | undefined): Promise<void> {
        const request = this.request;
        if (!request || this.completion.locked || !request.sourceObjects?.length) return;
        request.sourceObjectIds = ids;
        await this.inspect();
    }

    async commit(): Promise<void> {
        if (this.completion.locked || this.openingTarget) return;
        const request = this.request;
        const sessionId = this.ownedTarget?.sessionId ?? this.dependencies.sessionId();
        if (
            !request?.plan?.report?.executable ||
            request.status !== 'ready' ||
            (!request.documentSource && request.members.length === 0) ||
            request.error ||
            request.discovery.error ||
            requiredConversionIssues(request.plan).some((id) => !request.acknowledgedIssueIds.includes(id)) ||
            !request.target ||
            placementError(request.target, this.destinations()) ||
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
        const sampler = request.sourceFormat === 'nuedge.typhoon' ? 'Typhoon' : 'TX16W';
        this.dependencies.setStatus(`Importing ${sampler} disk set`);
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
                            ? `Converted ${sampler} document to ${request.destinationName}`
                            : `Imported ${request.members.length} ${sampler} disk image${request.members.length === 1 ? '' : 's'}`,
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
        if (!this.completion.locked) await this.inspect(this.request?.discovery.state === 'error');
    }

    private async discardPlan(request: Tx16wImportRequest): Promise<void> {
        const plan = request.plan;
        request.plan = null;
        request.acknowledgedIssueIds = [];
        if (plan?.state === 'READY') this.unreleasedPlans.add(plan.planId);
        for (const id of this.unreleasedPlans) {
            try {
                await this.dependencies.transport.releaseConversion(id);
            } catch (error) {
                if (!(error instanceof AxklibApiError && error.status === 404)) throw error;
            }
            this.unreleasedPlans.delete(id);
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
        if (request && (request.plan?.state === 'READY' || this.unreleasedPlans.size) && !this.completion.locked)
            await this.discardPlan(request);
        for (const member of request?.members ?? []) {
            await this.releaseMemberUpload(member);
        }
        await this.companions.releaseDebt();
        if (this.ownedTarget) {
            await this.dependencies.transport.closeImage(this.ownedTarget.sessionId);
            this.ownedTarget = null;
        }
        if (this.request === request) {
            this.request = null;
            this.companions.reset();
        }
    }

    private async releaseMemberUpload(member: Tx16wImportMember): Promise<void> {
        const pending = this.releasingMembers.get(member);
        if (pending) return pending;
        const upload = member.upload;
        if (!upload) return;
        const release = (async () => {
            try {
                await this.dependencies.transport.releaseClientUpload(upload);
            } catch (error) {
                if (!(error instanceof AxklibApiError && error.status === 404)) throw error;
            }
            member.upload = null;
            member.resolvedSource = null;
        })();
        this.releasingMembers.set(member, release);
        try {
            await release;
        } finally {
            this.releasingMembers.delete(member);
        }
    }

    private member(source: ClientUploadSource | FileLocation): Tx16wImportMember {
        return {
            id: this.nextMemberId++,
            source,
            sourceName: sourceName(source),
            resolvedSource: isFileLocation(source) ? source : null,
            upload: null,
            origin: 'SELECTED',
            sourceIdentity: tx16wSourceKey(source),
        };
    }

    private async inspect(forceDiscovery = false, typing = false): Promise<void> {
        const request = this.request;
        const sessionId = this.ownedTarget?.sessionId ?? this.dependencies.sessionId();
        if (!request || this.openingTarget) return;
        this.abortController?.abort();
        const controller = new AbortController();
        this.abortController = controller;
        request.error = '';
        request.status = 'inspecting';
        try {
            await this.discardPlan(request);
            if (controller.signal.aborted || this.request !== request) return;
            if (!request.target || placementError(request.target, this.destinations()) || sessionId === null) {
                request.status = 'waiting-target';
                return;
            }
            if (!request.documentSource && request.members.length === 0) {
                request.status = 'ready';
                return;
            }
            if (typing) {
                await new Promise<void>((resolve) => setTimeout(resolve, 250));
                if (controller.signal.aborted || this.request !== request) return;
            }
            if (request.sourceObjectIds?.length === 0 && request.sourceObjects) {
                request.status = 'ready';
                return;
            }
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
            await this.companions.resolve(request, controller.signal, forceDiscovery);
            if (controller.signal.aborted || this.request !== request) return;
            request.status = 'inspecting';
            request.progress = 1;
            if (!request.destinationProfile) await this.loadDestinationProfile(request);
            if (controller.signal.aborted || this.request !== request || !request.destinationProfile) return;
            const plan = await this.dependencies.transport.prepareConversion(
                {
                    profileId: request.destinationProfile,
                    source: request.documentSource ?? {
                        kind: 'SOURCES',
                        sources: request.members.map((member) => member.resolvedSource as InputFileLocation),
                    },
                    destinationSessionId: sessionId,
                    selection: { ...request.target },
                    options: {
                        mode: request.importMode,
                        ...(request.sourceObjectIds?.length ? { sourceObjectIds: [...request.sourceObjectIds] } : {}),
                    },
                    fidelity: request.fidelity,
                    capacityPolicy: this.completion.capacity.policy(),
                },
                controller.signal,
            );
            if (controller.signal.aborted || this.request !== request) {
                await this.dependencies.transport.releaseConversion(plan.planId);
                return;
            }
            request.sourceObjects = plan.report?.sourceObjects;
            if (request.sourceObjectIds?.length === 0) {
                await this.dependencies.transport.releaseConversion(plan.planId);
                if (controller.signal.aborted || this.request !== request) return;
            } else request.plan = plan;
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

function admittedSources(files: (ClientUploadSource | FileLocation)[]): (ClientUploadSource | FileLocation)[] {
    const seen = new Set<string | object>();
    return files.filter((source) => {
        if (tx16wDiskMediaType(sourceName(source)) === null || seen.has(tx16wSourceKey(source))) return false;
        seen.add(tx16wSourceKey(source));
        return true;
    });
}

function isFileLocation(source: ClientUploadSource | FileLocation): source is FileLocation {
    return 'kind' in source && source.kind === 'server-file';
}

function sourceName(source: ClientUploadSource | FileLocation): string {
    return isFileLocation(source)
        ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName)
        : source.name;
}
