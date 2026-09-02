import { AuditionController, type AuditionState } from '../../lib/audio/auditionController';
import { inspectorSelectionStopsPlayback } from '../../lib/audio/playbackSelection';
import { matchesSearch, playbackRowVisible } from '../../lib/auditionVisibility';
import {
    auditionableSampleBankIds,
    auditionableSampleIds,
    isStandaloneSample,
    stereoSampleIds,
} from '../../lib/sampleRelationships';
import type { ImageTransport, SamplerRelationship } from '../../lib/transport';
import type {
    Program,
    ProgramSampleSelectRow,
    SampleStructureItem,
    WaveDataItem,
    WorkspaceView,
} from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';

export interface LaneQueries {
    primary: string;
    secondary: string;
    tertiary: string;
}

type PreviewTarget = { kind: 'wave-data'; objectId: string; itemId: string } | { kind: 'sample'; objectId: string };

type CompanionRetry = { kind: 'audition'; objectId: string } | { kind: 'sample-bank'; bankId: string };

interface AuditionWorkflowDependencies {
    transport: ImageTransport;
    catalog: CatalogWorkflow;
    sessionId: () => number | null;
    workspaceView: () => WorkspaceView;
    setWorkspaceView: (view: WorkspaceView) => void;
    setInspectorOpen: (open: boolean) => void;
    setStatus: (status: string) => void;
    requestCompanionDisks: (retry: CompanionRetry) => void;
}

interface AuditionabilityIndex {
    relationships: readonly SamplerRelationship[];
    waveData: readonly WaveDataItem[];
    sampleBanks: readonly SampleStructureItem[];
    samples: readonly SampleStructureItem[];
    objectIds: Set<string>;
    sampleIds: Set<string>;
    sampleBankIds: Set<string>;
    stereoIds: Set<string>;
}

export class AuditionWorkflow {
    state = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    autoplay = $state(false);
    showOnlyStandaloneSamples = $state(true);
    playingSampleBankId = $state('');
    sampleBankPreviewMemberId = $state('');
    laneQueries = $state<Record<WorkspaceView, LaneQueries>>({
        programs: { primary: '', secondary: '', tertiary: '' },
        sequences: { primary: '', secondary: '', tertiary: '' },
        'sample-banks': { primary: '', secondary: '', tertiary: '' },
        samples: { primary: '', secondary: '', tertiary: '' },
        'wave-data': { primary: '', secondary: '', tertiary: '' },
    });

    private readonly controller: AuditionController;
    private pendingObjectId = '';
    private sampleBankPlaybackGeneration = 0;
    private readonly previewQueue: { target: PreviewTarget; generation: number }[] = [];
    private readonly previewPending = new Set<string>();
    private readonly previewFailed = new Set<string>();
    private previewInflight = 0;
    private previewGeneration = 0;
    private auditionabilityCache?: AuditionabilityIndex;

    constructor(private readonly dependencies: AuditionWorkflowDependencies) {
        this.controller = new AuditionController(dependencies.transport, (state) => {
            this.state = state;
            if (
                state.status === 'playing' &&
                state.objectId &&
                this.playingSampleBankId &&
                dependencies.catalog
                    .membersForBank(this.playingSampleBankId)
                    .some((member) => member.objectId === state.objectId)
            ) {
                this.sampleBankPreviewMemberId = state.objectId;
            }
            if (state.status === 'failed' && state.error) {
                dependencies.setStatus(state.error);
                if (
                    state.errorCode === 'companion_disks_required' &&
                    state.objectId &&
                    state.objectId === this.pendingObjectId
                ) {
                    dependencies.requestCompanionDisks({ kind: 'audition', objectId: state.objectId });
                }
                this.pendingObjectId = '';
            } else if (state.status === 'playing') {
                this.pendingObjectId = '';
            }
        });
    }

    get auditionableSampleObjectIds(): Set<string> {
        return this.auditionabilityIndex().sampleIds;
    }

    get auditionableObjectIds(): Set<string> {
        return this.auditionabilityIndex().objectIds;
    }

    get auditionableSampleBankObjectIds(): Set<string> {
        return this.auditionabilityIndex().sampleBankIds;
    }

    get stereoSampleObjectIds(): Set<string> {
        return this.auditionabilityIndex().stereoIds;
    }

    private auditionabilityIndex(): AuditionabilityIndex {
        const catalog = this.dependencies.catalog;
        const cached = this.auditionabilityCache;
        if (
            cached?.relationships === catalog.relationships &&
            cached.waveData === catalog.waveData &&
            cached.sampleBanks === catalog.sampleBanks &&
            cached.samples === catalog.samples
        ) {
            return cached;
        }
        const sampleIds = auditionableSampleIds(catalog.relationships, catalog.waveData);
        const result = {
            relationships: catalog.relationships,
            waveData: catalog.waveData,
            sampleBanks: catalog.sampleBanks,
            samples: catalog.samples,
            sampleIds,
            sampleBankIds: auditionableSampleBankIds(
                catalog.relationships,
                catalog.sampleBanks,
                catalog.samples,
                sampleIds,
            ),
            stereoIds: stereoSampleIds(catalog.relationships, catalog.waveData),
            objectIds: new Set([...sampleIds, ...catalog.waveData.map((item) => item.objectKey)]),
        };
        this.auditionabilityCache = result;
        return result;
    }

    get active(): boolean {
        return this.state.status === 'preparing' || this.state.status === 'playing';
    }

    get label(): string {
        if (!this.state.objectId) return '';
        const catalog = this.dependencies.catalog;
        const sample = catalog.samples.find((item) => item.objectId === this.state.objectId);
        if (this.playingSampleBankId) {
            const bank = catalog.sampleBanks.find((item) => item.objectId === this.playingSampleBankId);
            return [bank?.name, sample?.name].filter(Boolean).join(' / ');
        }
        return sample?.name ?? catalog.waveData.find((item) => item.objectKey === this.state.objectId)?.name ?? '';
    }

    selectProgram(program: Program): void {
        this.dependencies.catalog.selectedProgramId = program.objectId;
        this.setEditorObject(program.objectId);
        void this.inspectObject(program.objectId);
    }

    async selectBank(item: SampleStructureItem, playAfterSelection = this.autoplay): Promise<void> {
        const catalog = this.dependencies.catalog;
        catalog.selectedBankId = item.objectId;
        this.resetSampleBankPreview(item.objectId);
        catalog.selectedBankMemberId = '';
        catalog.selectedBankWaveDataId = '';
        this.setEditorObject(item.objectId);
        await this.inspectObject(item.objectId);
        if (
            playAfterSelection &&
            this.dependencies.workspaceView() === 'sample-banks' &&
            catalog.selectedBankId === item.objectId &&
            this.auditionableSampleBankObjectIds.has(item.objectId)
        ) {
            await this.playSampleBank(item);
        }
    }

    async selectSample(item: SampleStructureItem, playAfterSelection = this.autoplay): Promise<void> {
        const catalog = this.dependencies.catalog;
        catalog.selectedSampleId = item.objectId;
        catalog.selectedSampleWaveDataId = '';
        this.setEditorObject(item.objectId);
        await this.inspectObject(item.objectId);
        if (
            playAfterSelection &&
            this.dependencies.workspaceView() === 'samples' &&
            catalog.selectedSampleId === item.objectId &&
            this.auditionableSampleObjectIds.has(item.objectId)
        ) {
            this.playObject(item.objectId);
        }
    }

    async selectBankMember(item: SampleStructureItem, playAfterSelection = this.autoplay): Promise<void> {
        const catalog = this.dependencies.catalog;
        catalog.selectedBankMemberId = item.objectId;
        catalog.selectedBankWaveDataId = '';
        this.setEditorObject(item.objectId);
        await this.inspectObject(item.objectId);
        if (
            playAfterSelection &&
            this.dependencies.workspaceView() === 'sample-banks' &&
            catalog.selectedBankMemberId === item.objectId &&
            this.auditionableSampleObjectIds.has(item.objectId)
        ) {
            this.playObject(item.objectId);
        }
    }

    selectAssignment(row: ProgramSampleSelectRow): void {
        if (row.navigable && row.targetObjectId) void this.inspectObject(row.targetObjectId);
    }

    async selectWaveData(item: WaveDataItem, playAfterSelection = this.autoplay): Promise<void> {
        const catalog = this.dependencies.catalog;
        const view = this.dependencies.workspaceView();
        if (view === 'sample-banks') catalog.selectedBankWaveDataId = item.objectKey;
        else if (view === 'samples') catalog.selectedSampleWaveDataId = item.objectKey;
        else if (view === 'wave-data') catalog.selectedWaveDataId = item.objectKey;
        this.setEditorObject(item.objectKey);
        this.requestWaveformPreview(item);
        await this.inspectObject(item.objectKey);
        const selectionStillActive =
            (view === 'sample-banks' && catalog.selectedBankWaveDataId === item.objectKey) ||
            (view === 'samples' && catalog.selectedSampleWaveDataId === item.objectKey) ||
            (view === 'wave-data' && catalog.selectedWaveDataId === item.objectKey);
        if (playAfterSelection && selectionStillActive) this.playObject(item.objectKey);
    }

    resetPreviewQueue(): void {
        this.previewGeneration += 1;
        this.previewQueue.length = 0;
        this.previewPending.clear();
        this.previewFailed.clear();
        this.dependencies.catalog.samplePreviewStates = {};
    }

    requestWaveformPreview(item: WaveDataItem): void {
        const sessionId = this.dependencies.sessionId();
        if (
            sessionId === null ||
            item.previewState === 'ready' ||
            this.previewPending.has(item.objectKey) ||
            this.previewFailed.has(item.objectKey)
        ) {
            return;
        }
        this.previewPending.add(item.objectKey);
        const catalog = this.dependencies.catalog;
        catalog.waveData = catalog.waveData.map((candidate) =>
            candidate.id === item.id ? { ...candidate, previewState: 'loading' } : candidate,
        );
        this.previewQueue.push({
            target: { kind: 'wave-data', objectId: item.objectKey, itemId: item.id },
            generation: this.previewGeneration,
        });
        this.drainPreviewQueue();
    }

    requestSampleWaveformPreview(item: SampleStructureItem): void {
        const catalog = this.dependencies.catalog;
        const state = catalog.samplePreviewStates[item.objectId];
        if (
            this.dependencies.sessionId() === null ||
            state?.previewState === 'ready' ||
            this.previewPending.has(item.objectId) ||
            this.previewFailed.has(item.objectId)
        ) {
            return;
        }
        this.previewPending.add(item.objectId);
        catalog.samplePreviewStates = {
            ...catalog.samplePreviewStates,
            [item.objectId]: { preview: null, previewState: 'loading' },
        };
        this.previewQueue.push({
            target: { kind: 'sample', objectId: item.objectId },
            generation: this.previewGeneration,
        });
        this.drainPreviewQueue();
    }

    async playWaveData(item: WaveDataItem): Promise<void> {
        await this.selectWaveData(item, false);
        this.playObject(item.objectKey);
    }

    async playSample(item: SampleStructureItem): Promise<void> {
        if (!this.auditionableSampleObjectIds.has(item.objectId)) {
            this.dependencies.setStatus('This Sample has no confirmed Wave Data to audition');
            return;
        }
        await (this.dependencies.workspaceView() === 'sample-banks'
            ? this.selectBankMember(item, false)
            : this.selectSample(item, false));
        this.playObject(item.objectId);
    }

    async playSampleBank(item: SampleStructureItem): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null) return;
        const catalog = this.dependencies.catalog;
        if (catalog.selectedBankId !== item.objectId) await this.selectBank(item, false);
        if (this.dependencies.sessionId() === null) return;
        const members = catalog.membersForBank(item.objectId);
        const sampleIds = this.auditionableSampleObjectIds;
        const unavailableMember = members.find((member) => !sampleIds.has(member.objectId));
        if (unavailableMember) {
            this.sampleBankPreviewMemberId = unavailableMember.objectId;
            this.dependencies.setStatus(
                `Sample Bank audition requires playable Wave Data for ${unavailableMember.name}`,
            );
            return;
        }
        const memberIds = members.map((member) => member.objectId);
        if (memberIds.length === 0) {
            this.sampleBankPreviewMemberId = '';
            this.dependencies.setStatus('This Sample Bank has no playable Samples');
            return;
        }
        const generation = ++this.sampleBankPlaybackGeneration;
        this.playingSampleBankId = item.objectId;
        this.sampleBankPreviewMemberId = memberIds[0] ?? '';
        this.controller.playSequence(
            sessionId,
            memberIds,
            (result) => {
                if (generation !== this.sampleBankPlaybackGeneration) return;
                this.playingSampleBankId = '';
                this.resetSampleBankPreview(item.objectId);
                if (result.status === 'failed') {
                    this.dependencies.setStatus(result.error);
                    if (result.errorCode === 'companion_disks_required') {
                        this.dependencies.requestCompanionDisks({ kind: 'sample-bank', bankId: item.objectId });
                    }
                } else if (result.status === 'completed' && result.playedCount === 0) {
                    this.dependencies.setStatus('This Sample Bank has no playable Samples');
                }
            },
            item.objectId,
        );
    }

    async playContainedWaveData(item: WaveDataItem): Promise<void> {
        await this.selectWaveData(item, false);
        this.playObject(item.objectKey);
    }

    playObject(objectId: string): void {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null) return;
        if (!this.auditionableObjectIds.has(objectId)) {
            this.dependencies.setStatus('This Sample has no confirmed Wave Data to audition');
            return;
        }
        this.cancelSampleBankPlayback();
        this.pendingObjectId = objectId;
        void this.controller.play(sessionId, objectId);
    }

    prefetchObject(objectId: string): void {
        const sessionId = this.dependencies.sessionId();
        if (sessionId !== null && this.auditionableObjectIds.has(objectId)) {
            void this.controller.prefetch(sessionId, objectId);
        }
    }

    seekWaveData(item: WaveDataItem, ratio: number): void {
        void this.selectWaveData(item);
        if (this.state.objectId === item.objectKey) {
            const absoluteFrame = Math.floor(item.object.storedFrameCount * ratio);
            const relativeFrame = Math.max(
                0,
                Math.min(item.object.waveLengthFrames - 1, absoluteFrame - item.object.waveStartFrame),
            );
            this.controller.seek(relativeFrame);
        }
    }

    updateLaneQuery(view: WorkspaceView, lane: keyof LaneQueries, value: string): void {
        this.laneQueries[view][lane] = value;
        if (view === this.dependencies.workspaceView() && this.active && !this.currentPlaybackRowVisible()) {
            void this.stop();
        }
    }

    updateShowOnlyStandaloneSamples(enabled: boolean): void {
        this.showOnlyStandaloneSamples = enabled;
        if (!enabled) return;
        const catalog = this.dependencies.catalog;
        const selected = catalog.samples.find((item) => item.objectId === catalog.selectedSampleId);
        if (!selected || isStandaloneSample(selected)) return;
        const hiddenIds = new Set([
            selected.objectId,
            ...catalog.waveDataForSample(selected.objectId).map((item) => item.objectKey),
        ]);
        if (this.state.objectId && hiddenIds.has(this.state.objectId)) void this.stop();
        catalog.clearSampleSelection();
    }

    selectWorkspaceView(view: WorkspaceView): void {
        if (this.dependencies.workspaceView() === view) return;
        if (this.active) void this.stop();
        this.dependencies.setWorkspaceView(view);
    }

    async navigateToObject(objectId: string): Promise<WorkspaceView | null> {
        const catalog = this.dependencies.catalog;
        const program = catalog.programs.find((item) => item.objectId === objectId);
        const sequence = catalog.sequences.find((item) => item.objectId === objectId);
        const bank = catalog.sampleBanks.find((item) => item.objectId === objectId);
        const sample = catalog.samples.find((item) => item.objectId === objectId);
        const waveData = catalog.waveData.find((item) => item.objectKey === objectId);
        const view: WorkspaceView | null = program
            ? 'programs'
            : sequence
              ? 'sequences'
              : bank
                ? 'sample-banks'
                : sample
                  ? 'samples'
                  : waveData
                    ? 'wave-data'
                    : null;
        if (!view) return null;

        await this.stop();
        this.laneQueries[view].primary = '';
        if (sample && !isStandaloneSample(sample)) this.showOnlyStandaloneSamples = false;
        this.dependencies.setWorkspaceView(view);

        if (program) this.selectProgram(program);
        else if (sequence) {
            catalog.selectedSequenceId = sequence.objectId;
            catalog.editorObjectIds.sequences = sequence.objectId;
            catalog.inspectorObjectId = sequence.objectId;
            this.dependencies.setInspectorOpen(true);
        } else if (bank) await this.selectBank(bank, false);
        else if (sample) {
            await this.selectSample(sample, false);
            this.requestSampleWaveformPreview(sample);
        } else if (waveData) await this.selectWaveData(waveData, false);
        return view;
    }

    resetSampleBankPreview(bankId = this.dependencies.catalog.selectedBankId): void {
        this.sampleBankPreviewMemberId = bankId
            ? (this.dependencies.catalog.membersForBank(bankId)[0]?.objectId ?? '')
            : '';
    }

    stop(): Promise<void> {
        this.cancelSampleBankPlayback();
        return this.controller.stop();
    }

    invalidateSession(sessionId: number): Promise<void> {
        return this.controller.invalidateSession(sessionId);
    }

    dispose(): Promise<void> {
        return this.controller.dispose();
    }

    private setEditorObject(objectId: string): void {
        this.dependencies.catalog.editorObjectIds[this.dependencies.workspaceView()] = objectId;
    }

    private inspectObject(objectId: string): Promise<void> {
        const stopPlayback =
            Boolean(this.playingSampleBankId) || inspectorSelectionStopsPlayback(this.state.objectId, objectId);
        this.dependencies.catalog.inspectorObjectId = objectId;
        this.dependencies.setInspectorOpen(true);
        return stopPlayback ? this.stop() : Promise.resolve();
    }

    private cancelSampleBankPlayback(): void {
        const bankId = this.playingSampleBankId || this.dependencies.catalog.selectedBankId;
        this.sampleBankPlaybackGeneration += 1;
        this.playingSampleBankId = '';
        this.resetSampleBankPreview(bankId);
    }

    private currentPlaybackRowVisible(): boolean {
        const catalog = this.dependencies.catalog;
        const view = this.dependencies.workspaceView();
        const queries = this.laneQueries[view];
        const bankMembers = catalog.selectedBankId ? catalog.membersForBank(catalog.selectedBankId) : [];
        const memberWaveData = catalog.selectedBankMemberId
            ? catalog.waveDataForSample(catalog.selectedBankMemberId)
            : [];
        const sampleWaveData = catalog.selectedSampleId ? catalog.waveDataForSample(catalog.selectedSampleId) : [];
        const visibleSampleBankIds =
            view === 'sample-banks'
                ? catalog.sampleBanks
                      .filter((item) => matchesSearch(item.name, queries.primary))
                      .map((item) => item.objectId)
                : [];
        const visibleSampleIds =
            view === 'sample-banks'
                ? bankMembers.filter((item) => matchesSearch(item.name, queries.secondary)).map((item) => item.objectId)
                : view === 'samples'
                  ? catalog.samples
                        .filter(
                            (item) =>
                                (!this.showOnlyStandaloneSamples || isStandaloneSample(item)) &&
                                matchesSearch(item.name, queries.primary),
                        )
                        .map((item) => item.objectId)
                  : [];
        const visibleWaveDataIds =
            view === 'sample-banks'
                ? memberWaveData
                      .filter((item) => matchesSearch(item.name, queries.tertiary))
                      .map((item) => item.objectKey)
                : view === 'samples'
                  ? sampleWaveData
                        .filter((item) => matchesSearch(item.name, queries.secondary))
                        .map((item) => item.objectKey)
                  : view === 'wave-data'
                    ? catalog.waveData
                          .filter((item) => matchesSearch(item.name, queries.primary))
                          .map((item) => item.objectKey)
                    : [];
        return playbackRowVisible({
            view,
            playingSampleBankId: this.playingSampleBankId,
            playingObjectId: this.state.objectId,
            visibleSampleBankIds,
            visibleSampleIds,
            visibleWaveDataIds,
        });
    }

    private drainPreviewQueue(): void {
        while (this.previewInflight < 2 && this.previewQueue.length > 0) {
            const sessionId = this.dependencies.sessionId();
            if (sessionId === null) return;
            const queued = this.previewQueue.shift();
            if (!queued) return;
            const { target, generation } = queued;
            this.previewInflight += 1;
            void this.dependencies.transport
                .preview(sessionId, target.objectId, 1024)
                .then((preview) => {
                    if (this.dependencies.sessionId() !== sessionId || this.previewGeneration !== generation) return;
                    const catalog = this.dependencies.catalog;
                    if (target.kind === 'wave-data') {
                        const lane = preview.lanes[0];
                        if (!lane || lane.sourceObjectId !== target.objectId) {
                            throw new Error('Wave Data preview did not return its physical waveform lane');
                        }
                        catalog.waveData = catalog.waveData.map((candidate) =>
                            candidate.id === target.itemId
                                ? { ...candidate, waveform: lane.bins, previewState: 'ready' }
                                : candidate,
                        );
                    } else {
                        catalog.samplePreviewStates = {
                            ...catalog.samplePreviewStates,
                            [target.objectId]: { preview, previewState: 'ready' },
                        };
                    }
                })
                .catch((error) => {
                    if (this.dependencies.sessionId() !== sessionId || this.previewGeneration !== generation) return;
                    this.previewFailed.add(target.objectId);
                    const catalog = this.dependencies.catalog;
                    if (target.kind === 'wave-data') {
                        catalog.waveData = catalog.waveData.map((candidate) =>
                            candidate.id === target.itemId ? { ...candidate, previewState: 'failed' } : candidate,
                        );
                    } else {
                        catalog.samplePreviewStates = {
                            ...catalog.samplePreviewStates,
                            [target.objectId]: { preview: null, previewState: 'failed' },
                        };
                    }
                    this.dependencies.setStatus(userFacingMessage(error));
                })
                .finally(() => {
                    if (this.previewGeneration === generation) this.previewPending.delete(target.objectId);
                    this.previewInflight -= 1;
                    this.drainPreviewQueue();
                });
        }
    }
}
