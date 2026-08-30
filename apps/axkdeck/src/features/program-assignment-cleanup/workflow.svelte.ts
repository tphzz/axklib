import type {
    ImageTransport,
    ProgramAssignmentCleanupCandidate,
    ProgramAssignmentCleanupInspection,
    ProgramAssignmentCleanupResult,
    ProgramAssignmentCleanupSelection,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { JobController } from '../jobs/actions';

export interface ProgramAssignmentCleanupRow extends ProgramAssignmentCleanupCandidate {
    selected: boolean;
}

export interface ProgramAssignmentCleanupRequest {
    volumeName: string;
    inspection: ProgramAssignmentCleanupInspection | null;
    rows: ProgramAssignmentCleanupRow[];
    loading: boolean;
    busy: boolean;
    error: string;
}

interface Dependencies {
    transport: ImageTransport;
    jobs: JobController;
    sessionId: () => number | null;
    activeVolumeId: () => string;
    selectedSource: () => DiskTreeItem;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class ProgramAssignmentCleanupWorkflow {
    request = $state<ProgramAssignmentCleanupRequest | null>(null);
    private generation = 0;

    constructor(private readonly dependencies: Dependencies) {}

    async open(): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        const source = this.dependencies.selectedSource();
        if (
            sessionId === null ||
            source.kind !== 'volume' ||
            source.id !== this.dependencies.activeVolumeId() ||
            source.partitionIndex === undefined
        ) {
            this.dependencies.setStatus('Select a loaded volume first');
            return;
        }
        const generation = ++this.generation;
        this.request = {
            volumeName: source.name,
            inspection: null,
            rows: [],
            loading: true,
            busy: false,
            error: '',
        };
        try {
            const inspection = await this.dependencies.transport.inspectProgramAssignmentCleanup(sessionId, source.id);
            if (generation !== this.generation || !this.request) return;
            this.request = requestFromInspection(source.name, inspection);
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            this.request = { ...this.request, loading: false, error: userFacingMessage(error) };
        }
    }

    close(): void {
        if (!this.request?.busy) this.dispose();
    }

    dispose(): void {
        this.generation += 1;
        this.request = null;
    }

    setAssignmentSelected(programObjectId: string, assignmentOrdinal: number, selected: boolean): void {
        const request = this.request;
        if (!request || request.loading || request.busy) return;
        this.request = {
            ...request,
            rows: request.rows.map((row) =>
                row.programObjectId === programObjectId && row.assignmentOrdinal === assignmentOrdinal
                    ? { ...row, selected }
                    : row,
            ),
            error: '',
        };
    }

    setProgramSelected(programObjectId: string, selected: boolean): void {
        const request = this.request;
        if (!request || request.loading || request.busy) return;
        this.request = {
            ...request,
            rows: request.rows.map((row) => (row.programObjectId === programObjectId ? { ...row, selected } : row)),
            error: '',
        };
    }

    setAllSelected(selected: boolean): void {
        const request = this.request;
        if (!request || request.loading || request.busy) return;
        this.request = { ...request, rows: request.rows.map((row) => ({ ...row, selected })), error: '' };
    }

    async submit(): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        const source = this.dependencies.selectedSource();
        if (
            !request?.inspection ||
            request.loading ||
            request.busy ||
            sessionId === null ||
            source.kind !== 'volume' ||
            source.id !== request.inspection.contentScopeId ||
            source.partitionIndex === undefined
        ) {
            return;
        }
        const selections = selectedAssignments(request.rows);
        if (selections.length === 0) {
            this.request = { ...request, error: 'Select at least one unresolved assignment' };
            return;
        }

        const generation = ++this.generation;
        this.request = { ...request, busy: true, error: '' };
        const started = performance.now();
        try {
            const current = await this.dependencies.transport.inspectProgramAssignmentCleanup(sessionId, source.id);
            if (generation !== this.generation || !this.request) return;
            if (fingerprint(current) !== fingerprint(request.inspection)) {
                this.request = {
                    ...requestFromInspection(source.name, current),
                    error: 'Unresolved assignments changed. Review the updated list before cleaning.',
                };
                return;
            }
            await this.dependencies.invalidateSession(sessionId);
            this.dependencies.setStatus(`Cleaning ${selections.length} Program assignments`);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startProgramAssignmentCleanup(sessionId, source.id, selections),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed')
                throw new Error(completed.error ?? 'Program assignment cleanup did not complete');
            const result = completed.result as ProgramAssignmentCleanupResult | undefined;
            const cleaned = result?.cleanedAssignments?.length ?? selections.length;
            this.request = null;
            await this.dependencies.refreshSession({ partitionIndex: source.partitionIndex, volumeName: source.name });
            this.dependencies.reportTiming('program-assignment-cleanup', started, cleaned);
            this.dependencies.setStatus(`Cleaned ${cleaned} Program assignments`);
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, busy: false, error: message };
            this.dependencies.setStatus(message);
        }
    }
}

function requestFromInspection(
    volumeName: string,
    inspection: ProgramAssignmentCleanupInspection,
): ProgramAssignmentCleanupRequest {
    return {
        volumeName,
        inspection,
        rows: inspection.candidates.map((candidate) => ({ ...candidate, selected: candidate.defaultSelected })),
        loading: false,
        busy: false,
        error: '',
    };
}

function selectedAssignments(rows: ProgramAssignmentCleanupRow[]): ProgramAssignmentCleanupSelection[] {
    return rows.flatMap((row) =>
        row.selected ? [{ programObjectId: row.programObjectId, assignmentOrdinal: row.assignmentOrdinal }] : [],
    );
}

function fingerprint(inspection: ProgramAssignmentCleanupInspection): string {
    return JSON.stringify({
        imageId: inspection.imageId,
        revision: inspection.revision,
        contentScopeId: inspection.contentScopeId,
        candidates: inspection.candidates.map((candidate) => [
            candidate.programObjectId,
            candidate.programNumber,
            candidate.assignmentOrdinal,
            candidate.assignmentName,
            candidate.targetObjectType,
            candidate.reason,
            candidate.candidateTargetCount,
        ]),
    });
}
