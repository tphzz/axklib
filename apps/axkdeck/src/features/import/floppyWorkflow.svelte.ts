import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { ClientUploadLocation, FileLocation, DirectoryLocation } from '../../lib/storageLocations';
import type { FloppyInspection, FloppyInputLocation } from '../../lib/floppyImport';
import type { ImageSessionPackageImportPlan, PackageOpaqueSequenceDecision } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { shouldUseDirectComputerFileOperations } from '../../lib/fileOperationRouting';
import type { PackageImportDependencies } from './packageWorkflowTypes';
import {
    collectImportDestinations,
    importDestination,
    initialImportDestination,
    type ImportDestinationMode,
} from './packageDestinations';
import { ImportCompletion } from './importCompletion.svelte';
import { floppySelection, floppyVolumeName } from './floppySelection';

export type FloppySource = ClientUploadSource | FileLocation | DirectoryLocation;
interface Member {
    id: number;
    source: FloppySource;
    name: string;
    input: FloppyInputLocation | null;
    upload: ClientUploadLocation | null;
}
export interface FloppyRequest {
    target: DiskTreeItem | null;
    members: Member[];
    inspection: FloppyInspection | null;
    selected: string[];
    mode: ImportDestinationMode;
    partitionIndex: number | null;
    volumeName: string;
    newVolumeName: string;
    newVolumeNameEdited: boolean;
    status: 'choosing' | 'loading' | 'planning' | 'ready' | 'applying';
    error: string;
    plan: ImageSessionPackageImportPlan | null;
    dirty: boolean;
    renames: Record<string, string>;
    programSlots: Record<string, number>;
    opaqueSequenceActions: Record<string, PackageOpaqueSequenceDecision['action']>;
}
type Dependencies = PackageImportDependencies & {
    otherFormat: (
        format: FloppyInspection['format'],
        sources: (ClientUploadSource | FileLocation)[],
        target: DiskTreeItem | null,
    ) => Promise<void>;
};

export class FloppyImportWorkflow {
    request = $state<FloppyRequest | null>(null);
    readonly completion: ImportCompletion;
    private generation = 0;
    private nextId = 0;
    private controller: AbortController | null = null;
    private inspectionJob: number | null = null;
    constructor(private readonly dependencies: Dependencies) {
        this.completion = new ImportCompletion(dependencies.transport, dependencies.jobs);
    }
    get busy() {
        return !!this.request && ['loading', 'planning', 'applying'].includes(this.request.status);
    }
    get available() {
        return this.dependencies.mutationsAvailable?.() ?? false;
    }
    get directSource() {
        return shouldUseDirectComputerFileOperations(
            this.dependencies.isDesktop,
            this.dependencies.transport.connectionMode,
        );
    }
    destinations() {
        return collectImportDestinations(this.dependencies.sourceItems?.() ?? []);
    }
    selection() {
        return floppySelection(this.request?.inspection?.objects ?? [], this.request?.selected ?? []);
    }
    open(target: DiskTreeItem | null = null): void {
        if (this.request || !this.available) return;
        this.completion.reset();
        const initial = initialImportDestination(target ?? this.dependencies.selectedSource?.() ?? null);
        this.request = {
            target,
            members: [],
            inspection: null,
            selected: [],
            mode: initial?.mode ?? 'create',
            partitionIndex: initial?.partitionIndex ?? this.destinations().partitions[0]?.partitionIndex ?? null,
            volumeName: initial?.volumeName ?? '',
            newVolumeName: '',
            newVolumeNameEdited: false,
            status: 'choosing',
            error: '',
            plan: null,
            dirty: true,
            renames: {},
            programSlots: {},
            opaqueSequenceActions: {},
        };
    }
    async requestDroppedFiles(files: FloppySource[], target: DiskTreeItem | null = null): Promise<void> {
        if (this.request || !this.available) return;
        this.open(target);
        await this.add(files);
    }
    async chooseFiles(target: DiskTreeItem | null = null): Promise<void> {
        if (this.request || !this.available) return;
        this.open(target);
        if (this.directSource) await this.chooseWorkspace(true);
    }
    async chooseWorkspace(closeOnCancel = false): Promise<void> {
        const request = this.request;
        if (!request || this.busy || this.completion.locked) return;
        try {
            const selection = await this.dependencies.picker.chooseFloppySources({
                parentDialog: 'floppy-import',
            });
            if (this.request !== request) return;
            const files = Array.isArray(selection) ? selection : selection ? [selection] : null;
            if (files?.length) await this.add(files);
            else if (closeOnCancel && !request.members.length) await this.close();
        } catch (error) {
            if (this.request === request) request.error = userFacingMessage(error);
        }
    }
    async add(files: FloppySource[]): Promise<void> {
        const r = this.request;
        if (!r || this.busy || this.completion.locked || !files.length) return;
        if (
            r.members.length + files.length > 32 ||
            files.some((file) => !isDirectory(file) && !/\.(img|ima)$/i.test(sourceName(file)))
        ) {
            r.error = 'Choose floppy .img/.ima images or unpacked disk folders (up to 32 sources).';
            return;
        }
        const sources = [...r.members.map((member) => member.source), ...files];
        if (sources.some(isDirectory) && sources.some((source) => !isDirectory(source))) {
            r.error = 'Choose either disk images or unpacked disk folders.';
            return;
        }
        r.members.push(
            ...files.map((source) => ({
                id: ++this.nextId,
                source,
                name: sourceName(source),
                input: isLocation(source) ? source : null,
                upload: null,
            })),
        );
        await this.inspect();
    }
    async remove(id: number): Promise<void> {
        const r = this.request;
        if (!r || this.busy || this.completion.locked) return;
        const member = r.members.find((entry) => entry.id === id);
        r.members = r.members.filter((entry) => entry.id !== id);
        if (member?.upload) await this.dependencies.transport.releaseClientUpload(member.upload);
        await this.inspect();
    }
    setDestination(mode: ImportDestinationMode, partitionIndex: number | null, volumeName: string): void {
        const r = this.request;
        if (!r || this.busy || this.completion.locked) return;
        r.mode = mode;
        r.partitionIndex = partitionIndex;
        r.volumeName = volumeName.slice(0, 16);
        if (mode === 'create') {
            r.newVolumeName = r.volumeName;
            r.newVolumeNameEdited = true;
        }
        this.invalidate();
    }
    setPartition(partitionIndex: number | null): void {
        const r = this.request;
        if (!r || this.busy || this.completion.locked) return;
        r.partitionIndex = partitionIndex;
        if (r.mode === 'existing') r.volumeName = '';
        this.invalidate();
    }
    setMode(mode: ImportDestinationMode): void {
        const r = this.request;
        if (!r || this.busy || this.completion.locked || r.mode === mode) return;
        r.mode = mode;
        r.volumeName = mode === 'existing' ? '' : r.newVolumeName;
        this.invalidate();
    }
    private suggestVolumeName(r: FloppyRequest): void {
        if (r.newVolumeNameEdited) return;
        const source = r.members[0]?.source;
        const path = source
            ? isLocation(source)
                ? source.reference.relativePath || source.displayName
                : source.name
            : '';
        r.newVolumeName = source
            ? floppyVolumeName(r.inspection?.label ?? '', path, isDirectory(source) ? 'directory' : 'file')
            : '';
        if (r.mode === 'create') r.volumeName = r.newVolumeName;
    }
    toggle(key: string, checked: boolean): void {
        const r = this.request;
        if (!r || this.busy || this.completion.locked || this.selection().required.has(key)) return;
        if (!r.inspection?.objects.some((o) => o.objectKey === key && !o.exclusionReason)) return;
        r.selected = checked ? [...new Set([...r.selected, key])] : r.selected.filter((value) => value !== key);
        this.invalidate(true);
    }
    selectAll(checked: boolean): void {
        const r = this.request;
        if (!r || this.busy || this.completion.locked) return;
        r.selected = checked
            ? (r.inspection?.objects.filter((o) => !o.exclusionReason).map((o) => o.objectKey) ?? [])
            : [];
        this.invalidate(true);
    }
    rename(id: string, name: string): void {
        if (this.editable()) {
            this.request!.renames[id] = name;
            this.request!.dirty = true;
        }
    }
    programSlot(id: string, slot: number): void {
        if (this.editable() && Number.isInteger(slot) && slot >= 1 && slot <= 128) {
            this.request!.programSlots[id] = slot;
            this.request!.dirty = true;
        }
    }
    programStart(id: string, slot: number): void {
        const placement = this.request?.plan?.programSlotPlacements.find((p) => p.placementId === id);
        if (!placement || !Number.isInteger(slot) || slot < 1 || slot + placement.mappings.length - 1 > 128) return;
        placement.mappings.forEach((mapping, index) => this.programSlot(mapping.nodeId, slot + index));
    }
    opaqueSequenceAction(id: string, action: PackageOpaqueSequenceDecision['action']): void {
        if (this.editable()) {
            this.request!.opaqueSequenceActions[id] = action;
            this.request!.dirty = true;
        }
    }
    private editable() {
        return !!this.request && !this.busy && !this.completion.locked;
    }
    private invalidate(selectionChanged = false): void {
        const r = this.request;
        if (!r) return;
        const plan = r.plan;
        r.plan = null;
        r.dirty = true;
        r.error = '';
        r.renames = {};
        r.programSlots = {};
        if (selectionChanged) r.opaqueSequenceActions = {};
        if (plan) void this.dependencies.transport.releaseImagePackageImportPlan(plan.planToken).catch(() => undefined);
    }
    async review(): Promise<void> {
        const r = this.request,
            session = this.dependencies.sessionId();
        const token = r?.inspection?.inspectionToken;
        const destination = r && importDestination(r.mode, r.partitionIndex, r.volumeName);
        if (
            !r ||
            this.busy ||
            this.completion.locked ||
            !token ||
            !r.inspection?.complete ||
            !r.selected.length ||
            session === null ||
            !destination
        )
            return;
        const generation = ++this.generation;
        r.status = 'planning';
        r.error = '';
        r.dirty = true;
        try {
            for (let pass = 0; pass < 2; pass++) {
                const plan = await this.dependencies.transport.planFloppyImport(session, {
                    inspectionToken: token,
                    selectedObjectKeys: r.selected,
                    destination,
                    renames: Object.entries(r.renames)
                        .filter(([, name]) => name.trim())
                        .map(([nodeId, destinationName]) => ({
                            packageIndex: 0,
                            nodeId,
                            destinationName: destinationName.trim(),
                        })),
                    programSlotAssignments: Object.entries(r.programSlots).map(([nodeId, destinationSlot]) => ({
                        packageIndex: 0,
                        nodeId,
                        destinationSlot,
                    })),
                    opaqueSequenceDecisions: Object.entries(r.opaqueSequenceActions).map(([nodeId, action]) => ({
                        packageIndex: 0,
                        nodeId,
                        action,
                    })),
                });
                if (this.request !== r || generation !== this.generation) {
                    await this.dependencies.transport
                        .releaseImagePackageImportPlan(plan.planToken)
                        .catch(() => undefined);
                    return;
                }
                if (r.plan)
                    await this.dependencies.transport
                        .releaseImagePackageImportPlan(r.plan.planToken)
                        .catch(() => undefined);
                r.plan = plan;
                let added = false;
                for (const placement of plan.programSlotPlacements)
                    for (const mapping of placement.mappings) {
                        if (r.programSlots[mapping.nodeId] === undefined) {
                            r.programSlots[mapping.nodeId] = mapping.destinationSlot;
                            added = true;
                        }
                    }
                if (!added || !plan.programSlotPlacements.some((p) => !p.applied && p.mode !== 'UNAVAILABLE')) {
                    r.dirty = false;
                    break;
                }
            }
        } catch (error) {
            if (this.request === r && generation === this.generation) r.error = userFacingMessage(error);
        } finally {
            if (this.request === r && generation === this.generation) r.status = 'ready';
        }
    }
    async apply(): Promise<void> {
        const r = this.request,
            session = this.dependencies.sessionId();
        if (!r?.plan?.valid || r.dirty || !this.editable() || session === null) return;
        const destination = importDestination(r.mode, r.partitionIndex, r.volumeName);
        if (!destination) return;
        const token = r.plan.planToken;
        r.status = 'applying';
        r.error = '';
        try {
            await this.dependencies.invalidateSession(session);
            if (this.request !== r || this.dependencies.sessionId() !== session) return;
            await this.completion.run(
                () => this.dependencies.transport.startFloppyImport(token),
                async () => {
                    await this.releaseResources(r);
                    if (this.request !== r || this.dependencies.sessionId() !== session) return;
                    await this.dependencies.refreshSession({
                        partitionIndex: destination.partitionIndex,
                        volumeName: destination.volumeName,
                    });
                    this.dependencies.setStatus('Imported floppy into ' + destination.volumeName);
                    if (!this.completion.warnings.length && this.request === r) this.request = null;
                },
                (job) => {
                    if (job.progress?.label) this.dependencies.setStatus(job.progress.label);
                },
                r.plan.warnings.map((w) => w.message),
            );
            if (this.request === r && !this.completion.locked) {
                r.status = 'ready';
                r.error = this.completion.message;
            }
        } catch (error) {
            if (this.request === r) {
                r.status = 'ready';
                r.error = userFacingMessage(error);
            }
        }
    }
    async recover(): Promise<void> {
        const r = this.request;
        await this.completion.recover();
        if (r && this.request === r && !this.completion.locked) {
            r.status = 'ready';
            r.error = this.completion.message;
        }
    }
    async close(): Promise<void> {
        if (this.request?.status === 'applying' && this.completion.phase === 'idle') return;
        if (this.completion.canDismiss) await this.dispose();
    }
    async dispose(): Promise<void> {
        ++this.generation;
        this.controller?.abort();
        this.controller = null;
        if (this.inspectionJob !== null)
            await this.dependencies.transport.cancelJob(this.inspectionJob).catch(() => undefined);
        this.inspectionJob = null;
        const r = this.request;
        this.request = null;
        if (r) await this.releaseResources(r);
    }
    private async releaseResources(r: FloppyRequest): Promise<void> {
        if (r.plan) {
            await this.dependencies.transport.releaseImagePackageImportPlan(r.plan.planToken).catch(() => undefined);
            r.plan = null;
        }
        const token = r.inspection?.inspectionToken;
        if (token) {
            await this.dependencies.transport.releaseFloppyInspection(token).catch(() => undefined);
            r.inspection = null;
        }
        for (const member of r.members)
            if (member.upload) {
                await this.dependencies.transport.releaseClientUpload(member.upload).catch(() => undefined);
                member.upload = null;
            }
    }
    private async inspect(): Promise<void> {
        const r = this.request;
        if (!r) return;
        this.invalidate(true);
        if (r.inspection?.inspectionToken)
            await this.dependencies.transport
                .releaseFloppyInspection(r.inspection.inspectionToken)
                .catch(() => undefined);
        if (this.request !== r) return;
        r.inspection = null;
        this.suggestVolumeName(r);
        r.selected = [];
        if (!r.members.length) {
            r.status = 'choosing';
            return;
        }
        r.status = 'loading';
        r.error = '';
        const generation = ++this.generation;
        const controller = new AbortController();
        this.controller = controller;
        const current = () => this.request === r && generation === this.generation;
        try {
            for (const member of r.members) {
                if (member.input) continue;
                const upload = await this.dependencies.transport.uploadClientFile(
                    member.source as ClientUploadSource,
                    'DISK_IMAGE',
                    undefined,
                    controller.signal,
                );
                if (!current()) {
                    await this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined);
                    return;
                }
                member.upload = upload;
                member.input = upload;
            }
            const job = await this.dependencies.jobs.run(async () => {
                const started = await this.dependencies.transport.startFloppyInspection(r.members.map((m) => m.input!));
                this.inspectionJob = started.jobId;
                if (!current()) await this.dependencies.transport.cancelJob(started.jobId).catch(() => undefined);
                return started;
            });
            const inspection = job.result as FloppyInspection | undefined;
            if (!current()) {
                if (inspection?.inspectionToken)
                    await this.dependencies.transport
                        .releaseFloppyInspection(inspection.inspectionToken)
                        .catch(() => undefined);
                return;
            }
            if (job.status !== 'completed' || !inspection)
                throw new Error(job.error ?? 'Floppy inspection did not complete.');
            if (inspection.format !== 'A_SERIES') {
                if (r.members.some((member) => isDirectory(member.source)))
                    throw new Error('Folder import supports unpacked A-series floppy disks.');
                const sources = r.members
                        .map((m) => m.source)
                        .filter((source): source is ClientUploadSource | FileLocation => !isDirectory(source)),
                    target = r.target;
                await this.close();
                await this.dependencies.otherFormat(inspection.format, sources, target);
                return;
            }
            r.inspection = inspection;
            r.selected = inspection.objects.filter((o) => !o.exclusionReason).map((o) => o.objectKey);
            this.suggestVolumeName(r);
            r.status = 'ready';
        } catch (error) {
            if (current()) {
                r.status = 'ready';
                r.error = userFacingMessage(error);
            }
        } finally {
            if (current()) {
                this.inspectionJob = null;
                this.controller = null;
            }
        }
    }
}
function isDirectory(source: FloppySource): source is DirectoryLocation {
    return 'kind' in source && source.kind === 'server-directory';
}
function isLocation(source: FloppySource): source is FileLocation | DirectoryLocation {
    return 'kind' in source && (source.kind === 'server-file' || source.kind === 'server-directory');
}
function sourceName(source: FloppySource) {
    return isLocation(source) ? source.displayName : source.name;
}
