import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { CompanionSourceResolution } from '../../lib/conversions';
import { AxklibApiError } from '../../lib/httpErrors';
import {
    inputLocationKey,
    serverFileLocation,
    type InputFileLocation,
    type FileLocation,
    type ClientUploadLocation,
} from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { Tx16wImportMember, Tx16wImportRequest } from './tx16wWorkflow.svelte';

export interface Tx16wCompanionSearch {
    state: 'idle' | 'searching' | 'complete' | 'missing' | 'ambiguous' | 'manual' | 'error';
    message: string;
    error: string;
    requirements: CompanionSourceResolution['requirements'];
}
export const idleCompanionSearch = (): Tx16wCompanionSearch => ({
    state: 'idle',
    message: '',
    error: '',
    requirements: [],
});
class ClientSourceIdentity {}
const clientSourceIdentities = new WeakMap<object, object>();
export function tx16wSourceKey(source: ClientUploadSource | FileLocation): string | object {
    if ('kind' in source) return inputLocationKey(source);
    const key = source.identity ?? source;
    if (typeof key === 'string') return key;
    let identity = clientSourceIdentities.get(key);
    if (!identity) {
        identity = new ClientSourceIdentity();
        clientSourceIdentities.set(key, identity);
    }
    return identity;
}

type Candidate = { source: ClientUploadSource; upload: ClientUploadLocation };
export class Tx16wCompanionDiscovery {
    private completedMembers = '';
    private readonly exclusions = new Map<string | object, FileLocation | null>();
    private readonly cleanupDebt = new Set<ClientUploadLocation>();
    constructor(
        private readonly transport: ImageTransport,
        private readonly member: (source: ClientUploadSource | FileLocation) => Tx16wImportMember,
        private readonly reportError: (message: string) => void,
    ) {}
    reset(): void {
        this.completedMembers = '';
        this.exclusions.clear();
    }
    exclude(member: Tx16wImportMember): void {
        this.exclusions.set(
            member.sourceIdentity,
            member.resolvedSource?.kind === 'server-file' ? member.resolvedSource : null,
        );
    }
    include(source: ClientUploadSource | FileLocation): void {
        this.exclusions.delete(tx16wSourceKey(source));
    }
    async releaseDebt(): Promise<void> {
        let failure: unknown;
        for (const upload of this.cleanupDebt) {
            try {
                await this.transport.releaseClientUpload(upload);
                this.cleanupDebt.delete(upload);
            } catch (error) {
                if (error instanceof AxklibApiError && error.status === 404) this.cleanupDebt.delete(upload);
                else failure = error;
            }
        }
        if (failure) throw failure;
    }
    async resolve(request: Tx16wImportRequest, signal: AbortSignal, force: boolean): Promise<void> {
        if (request.documentSource || request.members.length === 0) return;
        const signature = request.members.map((member) => member.id).join(',');
        if (!force && signature === this.completedMembers) return;
        const search = request.discovery;
        search.state = 'searching';
        search.message = 'Looking for companion disks...';
        search.error = '';
        const temporary: Candidate[] = [];
        const adopted = new Set<ClientUploadLocation>();
        try {
            await this.releaseDebt();
            signal.throwIfAborted();
            const sources = request.members.map((member) => member.resolvedSource!);
            const exclusions = [...this.exclusions.values()].filter(
                (source): source is FileLocation => source !== null,
            );
            const first = request.members[0].source;
            const native =
                !('kind' in first) && this.transport.connectionMode === 'local'
                    ? first.nearbySources?.bind(first)
                    : undefined;
            let result = await this.transport.resolveCompanionSources(
                { sources, exclusions, searchNearby: sources[0].kind === 'server-file' },
                signal,
            );
            signal.throwIfAborted();
            if (result.requirements.length && native) {
                const nearby = await native(signal);
                signal.throwIfAborted();
                if (nearby.length > 1024) throw new Error('Companion discovery exceeds 1024 candidates');
                let bytes = 0;
                const seen = new Set(request.members.map((member) => member.sourceIdentity));
                for (const source of nearby) {
                    const key = tx16wSourceKey(source);
                    if (seen.has(key) || this.exclusions.has(key)) continue;
                    seen.add(key);
                    if (!Number.isSafeInteger(source.size) || source.size < 0 || source.size > 4 * 1024 * 1024)
                        throw new Error('Companion source exceeds 4 MiB');
                    bytes += source.size;
                    if (bytes > 64 * 1024 * 1024) throw new Error('Companion discovery exceeds 64 MiB');
                    signal.throwIfAborted();
                    const upload = await this.transport.uploadClientFile(source, 'DISK_IMAGE', () => undefined, signal);
                    temporary.push({ source, upload });
                    signal.throwIfAborted();
                }
                result = await this.transport.resolveCompanionSources(
                    {
                        sources,
                        candidates: temporary.map((candidate) => candidate.upload),
                        exclusions,
                        searchNearby: false,
                    },
                    signal,
                );
                signal.throwIfAborted();
            }
            const members = result.members.map((item) => {
                const location = item.source.kind === 'FILE' ? serverFileLocation(item.source.file, item.name) : null;
                const key = location
                    ? inputLocationKey(location)
                    : JSON.stringify(['upload', item.source.kind === 'UPLOAD' ? item.source.upload.uploadId : '']);
                const existing = request.members.find(
                    (member) => member.resolvedSource && inputLocationKey(member.resolvedSource) === key,
                );
                if (existing) return existing;
                if (location) return { ...this.member(location), origin: 'AUTO_ADDED' as const };
                const candidate = temporary.find((candidate) => inputLocationKey(candidate.upload) === key);
                if (!candidate) throw new Error('Companion resolution returned an unknown upload');
                adopted.add(candidate.upload);
                return {
                    ...this.member(candidate.source),
                    upload: candidate.upload,
                    resolvedSource: candidate.upload,
                    origin: 'AUTO_ADDED' as const,
                };
            });
            // The resolver may deduplicate explicitly selected filesystem identities.
            for (const old of request.members) {
                if (!members.some((member) => member.id === old.id) && old.upload) {
                    await this.transport.releaseClientUpload(old.upload);
                    old.upload = null;
                    old.resolvedSource = null;
                    signal.throwIfAborted();
                }
            }
            request.members = members;
            this.completedMembers = members.map((member) => member.id).join(',');
            search.requirements = result.requirements;
            const manual = sources[0].kind === 'client-upload' && !native;
            search.state = !result.requirements.length
                ? 'complete'
                : manual
                  ? 'manual'
                  : result.requirements.some((item) => item.state === 'AMBIGUOUS')
                    ? 'ambiguous'
                    : 'missing';
            search.message =
                search.state === 'complete'
                    ? ''
                    : search.state === 'manual'
                      ? 'Add all disks in this set. Uploaded files have no server folder.'
                      : search.state === 'ambiguous'
                        ? 'Multiple matching disks found. Add disks to choose a companion.'
                        : 'Missing companion files. Add the required disk to continue.';
        } catch (error) {
            if (signal.aborted) throw error;
            this.completedMembers = signature;
            search.state = 'error';
            search.error = userFacingMessage(error);
            search.message = 'Companion search incomplete. Retry or add disks explicitly.';
        } finally {
            for (const candidate of temporary) {
                if (
                    adopted.has(candidate.upload) &&
                    request.members.some(
                        (member) =>
                            member.upload && inputLocationKey(member.upload) === inputLocationKey(candidate.upload),
                    )
                )
                    continue;
                this.cleanupDebt.add(candidate.upload);
            }
            try {
                await this.releaseDebt();
            } catch (error) {
                const message = userFacingMessage(error);
                search.state = 'error';
                search.error = message;
                search.message = 'Companion cleanup incomplete. Retry or close to try cleanup again.';
                this.reportError(message);
            }
        }
    }
}
