import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { ClientUploadLocation, FileLocation, ImageLocation, InputFileLocation } from '../../lib/storageLocations';
import type { AudioImportTarget, ImageTransport, Tx16wImportInspection, Tx16wImportMode } from '../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { tx16wDiskMediaType } from '../../lib/tx16wImport';
import type { JobController } from '../jobs/actions';

export interface Tx16wVolumeOption {
    key: string;
    label: string;
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
    inspection: Tx16wImportInspection | null;
    status: 'waiting-target' | 'uploading' | 'inspecting' | 'ready' | 'importing';
    progress: number;
    error: string;
}

interface Tx16wImportDependencies {
    transport: ImageTransport;
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

    constructor(private readonly dependencies: Tx16wImportDependencies) {}

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
        return collectTx16wVolumeOptions(this.dependencies.sourceItems());
    }

    async requestDroppedFiles(
        files: (ClientUploadSource | FileLocation)[],
        target: AudioImportTarget | null = this.activeTarget(),
    ): Promise<void> {
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
        this.request = {
            members: admitted.map((source) => this.member(source)),
            target,
            importMode: 'HIERARCHY',
            inspection: null,
            status: target ? 'inspecting' : 'waiting-target',
            progress: 0,
            error: '',
        };
        if (target) await this.inspect();
    }

    async addFiles(files: (ClientUploadSource | FileLocation)[]): Promise<void> {
        const request = this.request;
        if (!request || request.status === 'importing') return;
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
        request.inspection = null;
        request.error = '';
        if (request.target) await this.inspect();
    }

    async removeMember(memberId: number): Promise<void> {
        const request = this.request;
        if (!request || request.status === 'importing') return;
        this.abortController?.abort();
        const index = request.members.findIndex((member) => member.id === memberId);
        if (index < 0) return;
        const [removed] = request.members.splice(index, 1);
        if (removed.upload)
            await this.dependencies.transport.releaseClientUpload(removed.upload).catch(() => undefined);
        request.inspection = null;
        request.error = '';
        if (request.members.length === 0) {
            await this.close();
        } else if (request.target) {
            await this.inspect();
        }
    }

    async selectTarget(target: AudioImportTarget): Promise<void> {
        const request = this.request;
        if (!request || request.status === 'importing') return;
        request.target = target;
        request.inspection = null;
        request.error = '';
        await this.inspect();
    }

    async selectImportMode(importMode: Tx16wImportMode): Promise<void> {
        const request = this.request;
        if (!request || request.status === 'importing' || request.importMode === importMode) return;
        request.importMode = importMode;
        request.inspection = null;
        request.error = '';
        if (request.target) await this.inspect();
    }

    async commit(): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        const sources = request?.members.map((member) => member.resolvedSource);
        if (
            !request?.inspection?.valid ||
            !sources?.every((source): source is InputFileLocation => source !== null) ||
            !request.target ||
            sessionId === null
        ) {
            return;
        }
        const target = request.target;
        const importMode = request.importMode;
        const objectCount = Object.values(request.inspection.counts).reduce((total, count) => total + count, 0);
        const started = performance.now();
        let submitted = false;
        request.status = 'importing';
        request.error = '';
        this.dependencies.setStatus('Importing TX16W disk set');
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                async () => {
                    const job = await this.dependencies.transport.startTx16wDiskSetImport(
                        sessionId,
                        sources,
                        target,
                        importMode,
                    );
                    submitted = true;
                    return job;
                },
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'TX16W import did not complete');
            this.dependencies.selectWorkspace('programs');
            await this.dependencies.refreshSession(target);
            this.dependencies.reportTiming('tx16w-disk-set-import', started, objectCount);
            this.dependencies.setStatus(
                `Imported ${request.members.length} TX16W disk image${request.members.length === 1 ? '' : 's'}`,
            );
            await this.close();
        } catch (error) {
            const message = userFacingMessage(error);
            if (submitted) {
                try {
                    await this.dependencies.refreshSession(target);
                    this.dependencies.setStatus(`Import result could not be confirmed; image refreshed: ${message}`);
                    await this.close();
                } catch (refreshError) {
                    request.status = 'ready';
                    request.error = `${message}. Refresh also failed: ${userFacingMessage(refreshError)}`;
                    this.dependencies.setStatus(request.error);
                }
            } else {
                request.status = 'ready';
                request.error = message;
                this.dependencies.setStatus(message);
            }
        }
    }

    async close(): Promise<void> {
        this.abortController?.abort();
        this.abortController = null;
        const uploads = this.request?.members.flatMap((member) => (member.upload ? [member.upload] : [])) ?? [];
        this.request = null;
        await Promise.all(
            uploads.map((upload) => this.dependencies.transport.releaseClientUpload(upload).catch(() => undefined)),
        );
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
        const sessionId = this.dependencies.sessionId();
        if (!request?.target || request.members.length === 0 || sessionId === null) return;
        this.abortController?.abort();
        const controller = new AbortController();
        this.abortController = controller;
        request.error = '';
        try {
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
            request.inspection = await this.dependencies.transport.inspectTx16wDiskSet(
                sessionId,
                request.members.map((member) => member.resolvedSource as InputFileLocation),
                request.target,
                request.importMode,
            );
            if (controller.signal.aborted || this.request !== request) return;
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
