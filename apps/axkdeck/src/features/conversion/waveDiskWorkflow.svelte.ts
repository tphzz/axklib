import { conversionRoutes, preferredConversionRoute } from '../../lib/conversionRoutes';
import {
    requiredConversionIssues,
    type ConversionFidelity,
    type ConversionInspection,
    type ConversionPlan,
    type ConversionProfile,
    type ConversionResult,
    type ConversionSource,
} from '../../lib/conversions';
import type { FileLocation } from '../../lib/storageLocations';
import type { ImageTransport, RetainedDownload } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { WaveSourceScope } from './waveSourceScope';
import { collectPages } from '../../lib/pagination';

type DocumentSource = Extract<ConversionSource, { kind: 'DOCUMENT' }>;
export type WaveRow = ConversionInspection['items'][number] & { location?: string };
type Transport = Pick<
    ImageTransport,
    | 'conversionProfiles'
    | 'inspectConversion'
    | 'prepareConversion'
    | 'executeConversion'
    | 'conversionExecution'
    | 'waitForJob'
    | 'releaseConversion'
    | 'deleteRetainedPackage'
>;
type Delivery = { kind: 'server'; file: FileLocation | null } | { kind: 'download'; filename: string };
export type WaveDiskRequest = {
    source: DocumentSource;
    sourceName: string;
    scopeKind: WaveSourceScope['kind'];
    profile?: ConversionProfile;
    rows: WaveRow[];
    selected: string[];
    fidelity: ConversionFidelity;
    delivery: Delivery;
    phase: 'inspecting' | 'idle' | 'preparing' | 'ready' | 'executing' | 'unconfirmed' | 'save-again';
    error: string;
    plan: ConversionPlan | null;
    retained?: RetainedDownload;
};

export function waveDiskRoutes(profiles: ConversionProfile[], sourceFormat: string) {
    return conversionRoutes(profiles, {
        sourceFormat,
        destinationFormat: 'yamaha.tx16w',
        outputKind: 'NEW_IMAGE',
        medium: 'fat12-720k',
        intent: 'WAVE_DATA',
        editorPanels: ['ordered-wave-selection', 'new-image-file'],
    }).filter(({ supported }) => supported);
}

export function filenameError(filename: string) {
    return !filename.trim() || !/^[^/\\:*?"<>|\x00-\x1f]+\.img$/i.test(filename) || filename.startsWith('.')
        ? 'Enter an .img filename without a directory.'
        : '';
}

export class WaveDiskWorkflow {
    request = $state<WaveDiskRequest | null>(null);
    private controller?: AbortController;
    private identity = 0;
    constructor(
        private readonly transport: Transport,
        private readonly publish: (download: RetainedDownload, name: string) => Promise<boolean>,
        private readonly setStatus: (status: string) => void,
    ) {}

    get locked() {
        return this.request && ['executing', 'unconfirmed', 'save-again'].includes(this.request.phase);
    }
    get canCreate() {
        const r = this.request;
        return (
            !!r?.plan?.report?.executable &&
            r.phase === 'ready' &&
            !requiredConversionIssues(r.plan).length &&
            (r.delivery.kind === 'server' ? !!r.delivery.file : !filenameError(r.delivery.filename))
        );
    }
    private invalidate() {
        this.controller?.abort();
        this.controller = new AbortController();
        return ++this.identity;
    }
    private release(plan: ConversionPlan | null | undefined) {
        if (plan) void this.transport.releaseConversion(plan.planId).catch(() => undefined);
    }
    close() {
        if (this.request?.phase === 'executing') return;
        this.invalidate();
        const r = this.request;
        if (r?.retained) void this.transport.deleteRetainedPackage(r.retained).catch(() => undefined);
        if (r?.phase !== 'unconfirmed') this.release(r?.plan);
        this.request = null;
    }
    async open(
        source: DocumentSource,
        sourceName: string,
        sourceFormat: string,
        profiles: ConversionProfile[],
        initialIds: string[],
        scope?: WaveSourceScope,
    ) {
        if (this.locked) return;
        this.close();
        const routes = waveDiskRoutes(profiles, sourceFormat);
        if (!routes.length) return;
        const gen = this.invalidate(),
            { signal } = this.controller!;
        this.request = {
            source,
            sourceName: [sourceName, scope?.label].filter(Boolean).join(' · '),
            scopeKind: scope?.kind ?? 'image',
            rows: [],
            selected: [],
            fidelity: 'BEST',
            delivery: { kind: 'server', file: null },
            phase: 'inspecting',
            plan: null,
            error: '',
        };
        try {
            let profile: ConversionProfile | undefined;
            const inspected = await collectPages(
                async (offset, limit) => {
                    const page = await this.transport.inspectConversion(
                        { source, sourceFormat, objectType: 'SMPL', offset, limit },
                        signal,
                    );
                    signal.throwIfAborted();
                    if (
                        page.documentId !== source.documentId ||
                        page.sourceFormat !== sourceFormat ||
                        page.offset !== offset
                    )
                        throw new Error('The source catalog changed. Reopen Convert to inspect it again.');
                    const matches = routes.filter(({ profile: p }) => page.profileIds.includes(p.id));
                    const match = preferredConversionRoute(matches)?.profile;
                    if (!match || (profile && match.id !== profile.id))
                        throw new Error('No compatible Wave disk converter is available for this source.');
                    profile = match;
                    return { items: page.items, totalCount: page.total };
                },
                { pageSize: 200, key: (row) => row.id, cancelled: () => signal.aborted },
            );
            const locations = await scope?.locations(signal);
            if (gen !== this.identity || !profile) return;
            const rows = locations
                ? inspected
                      .filter((row) => locations.has(row.id))
                      .map((row) => ({ ...row, location: locations.get(row.id) }))
                : inspected;
            const ids = new Set(rows.map((row) => row.id));
            const selected = [...new Set(initialIds)].filter((id) => ids.has(id));
            this.request = { ...this.request!, rows, profile, selected, phase: 'idle' };
            await this.prepare();
        } catch (reason) {
            if (gen === this.identity && this.request)
                this.request = { ...this.request, phase: 'idle', error: userFacingMessage(reason) };
        }
    }
    change(update: Partial<Pick<WaveDiskRequest, 'selected' | 'fidelity' | 'delivery'>>) {
        if (!this.request || this.locked || this.request.phase === 'inspecting') return;
        this.request = { ...this.request, ...update };
        void this.prepare();
    }
    async prepare() {
        const r = this.request;
        if (!r || this.locked || r.phase === 'inspecting') return;
        const gen = this.invalidate(),
            { signal } = this.controller!;
        this.release(r.plan);
        this.request = { ...r, plan: null, error: '', phase: 'idle' };
        if (!r.profile || !r.selected.length || (r.delivery.kind === 'server' && !r.delivery.file)) return;
        this.request = { ...this.request, phase: 'preparing' };
        try {
            const plan = await this.transport.prepareConversion(
                {
                    profileId: r.profile.id,
                    source: r.source,
                    destination: {
                        kind: 'NEW_IMAGE',
                        ...(r.delivery.kind === 'server' && r.delivery.file ? { file: r.delivery.file.reference } : {}),
                    },
                    options: { documentId: r.source.documentId, waveIds: [...r.selected] },
                    fidelity: r.fidelity,
                },
                signal,
            );
            if (gen !== this.identity) {
                this.release(plan);
                return;
            }
            this.request = { ...this.request!, plan, phase: 'ready' };
        } catch (reason) {
            if (gen === this.identity && this.request)
                this.request = { ...this.request, phase: 'idle', error: userFacingMessage(reason) };
        }
    }
    renameDownload(name: string) {
        const r = this.request;
        if (r?.delivery.kind === 'download' && !this.locked)
            this.request = { ...r, delivery: { kind: 'download', filename: name } };
    }
    async execute() {
        const r = this.request;
        if (!r?.plan || !(this.canCreate || r.phase === 'unconfirmed' || r.phase === 'save-again')) return;
        this.request = { ...r, error: '', phase: 'executing' };
        try {
            let retained = r.retained;
            let status = 'Created TX16W Wave disk';
            if (!retained) {
                const job =
                    r.phase === 'unconfirmed'
                        ? await this.transport.conversionExecution(r.plan.planId)
                        : await this.transport.executeConversion(r.plan, []);
                if (!job) {
                    // The server confirmed no job is bound. Keep its idempotency key for any explicit retry.
                    this.request = {
                        ...r,
                        phase: 'ready',
                        error: 'No execution was recorded. The reviewed disk image can be created again.',
                    };
                    return;
                }
                const result = await this.transport.waitForJob(job.jobId, () => undefined);
                if (result.status !== 'completed') {
                    if (!['failed', 'cancelled'].includes(result.status))
                        throw new Error('Execution status is not confirmed.');
                    this.release(r.plan);
                    this.request = {
                        ...r,
                        phase: 'idle',
                        plan: null,
                        error: result.error ?? 'Conversion did not complete.',
                    };
                    return;
                }
                const delivered = result.result as ConversionResult | undefined;
                if (delivered?.planId !== r.plan.planId)
                    throw new Error('The execution did not confirm the reviewed plan.');
                if (r.delivery.kind === 'server') {
                    const expected = r.delivery.file?.reference;
                    if (
                        !expected ||
                        !('file' in delivered) ||
                        delivered.file?.rootId !== expected.rootId ||
                        delivered.file.relativePath !== expected.relativePath
                    )
                        throw new Error('The execution did not confirm the reviewed output file.');
                    if (delivered.warnings?.length)
                        status += '. ' + delivered.warnings.map((warning) => warning.message).join(' ');
                } else {
                    retained = 'download' in delivered ? delivered.download : undefined;
                    if (!retained) throw new Error('The execution did not provide a Wave disk download.');
                    const target = r.plan.report?.destination;
                    if (
                        !target ||
                        !('kind' in target) ||
                        target.kind !== 'NEW_IMAGE' ||
                        retained.sizeBytes !== target.sizeBytes
                    )
                        throw new Error('The downloaded image does not match the reviewed disk size.');
                }
            }
            if (r.delivery.kind === 'download' && retained) {
                try {
                    if (!(await this.publish(retained, r.delivery.filename))) {
                        this.request = {
                            ...r,
                            retained,
                            phase: 'save-again',
                            error: 'Disk image created. Choose where to save it.',
                        };
                        return;
                    }
                } catch (reason) {
                    this.request = { ...r, retained, phase: 'save-again', error: userFacingMessage(reason) };
                    return;
                }
                await this.transport.deleteRetainedPackage(retained).catch(() => undefined);
            }
            this.release(r.plan);
            this.request = null;
            this.setStatus(status);
        } catch (reason) {
            this.request = { ...r, phase: 'unconfirmed', error: userFacingMessage(reason) };
        }
    }
}
