import type {
    ImageTransport,
    ProgramGenerationCandidate,
    ProgramGenerationInspection,
    ProgramGenerationResult,
    ProgramGenerationSelection,
} from '../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { JobController } from '../jobs/actions';

export interface ProgramGenerationRow extends ProgramGenerationCandidate {
    selected: boolean;
    programName: string;
    programNumber: number | null;
}

export interface ProgramGenerationRequest {
    volumeName: string;
    inspection: ProgramGenerationInspection | null;
    rows: ProgramGenerationRow[];
    loading: boolean;
    busy: boolean;
    error: string;
}

interface ProgramGenerationDependencies {
    transport: ImageTransport;
    jobs: JobController;
    sessionId: () => number | null;
    activeVolumeId: () => string;
    selectedSource: () => DiskTreeItem;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    selectProgram: (programNumber: number) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class ProgramGenerationWorkflow {
    request = $state<ProgramGenerationRequest | null>(null);
    private generation = 0;

    constructor(private readonly dependencies: ProgramGenerationDependencies) {}

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
            const inspected = await this.dependencies.transport.inspectProgramGeneration(sessionId, source.id);
            if (generation !== this.generation || !this.request) return;
            this.request = requestFromInspection(source.name, inspected);
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            this.request = { ...this.request, loading: false, error: userFacingMessage(error) };
        }
    }

    close(): void {
        if (this.request?.busy) return;
        this.dispose();
    }

    dispose(): void {
        this.generation += 1;
        this.request = null;
    }

    setSelected(targetObjectId: string, selected: boolean): void {
        const request = this.request;
        if (!request || request.loading || request.busy || !request.inspection) return;
        const rows = request.rows.map((row) => (row.targetObjectId === targetObjectId ? { ...row, selected } : row));
        this.request = { ...request, rows: packRows(rows, request.inspection.availableProgramNumbers), error: '' };
    }

    setAllSelected(selected: boolean): void {
        const request = this.request;
        if (!request || request.loading || request.busy || !request.inspection) return;
        const capacity = request.inspection.availableProgramNumbers.length;
        const rows = request.rows.map((row, index) => ({ ...row, selected: selected && index < capacity }));
        this.request = { ...request, rows: packRows(rows, request.inspection.availableProgramNumbers), error: '' };
    }

    setProgramName(targetObjectId: string, programName: string): void {
        const request = this.request;
        if (!request || request.loading || request.busy) return;
        this.request = {
            ...request,
            rows: request.rows.map((row) =>
                row.targetObjectId === targetObjectId ? { ...row, programName: programName.slice(0, 8) } : row,
            ),
            error: '',
        };
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
        const programs = selectedPrograms(request.rows);
        const invalidName = programs.find((program) => programNameError(program.programName));
        if (programs.length === 0 || invalidName) {
            this.request = {
                ...request,
                error: invalidName ? (programNameError(invalidName.programName) ?? '') : 'Select at least one target',
            };
            return;
        }

        const generation = ++this.generation;
        this.request = { ...request, busy: true, error: '' };
        const started = performance.now();
        try {
            const current = await this.dependencies.transport.inspectProgramGeneration(sessionId, source.id);
            if (generation !== this.generation || !this.request) return;
            if (inspectionFingerprint(current) !== inspectionFingerprint(request.inspection)) {
                this.request = {
                    ...requestFromInspection(source.name, current),
                    error: 'Generation candidates changed. Review the updated Programs before generating.',
                };
                return;
            }
            await this.dependencies.invalidateSession(sessionId);
            this.dependencies.setStatus(`Generating ${programs.length} Programs`);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startProgramGeneration(sessionId, source.id, programs),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Program generation did not complete');
            }
            const result = completed.result as ProgramGenerationResult | undefined;
            const firstProgramNumber = result?.createdPrograms[0]?.programNumber ?? programs[0]?.programNumber;
            this.request = null;
            this.dependencies.selectWorkspace('programs');
            await this.dependencies.refreshSession({ partitionIndex: source.partitionIndex, volumeName: source.name });
            if (firstProgramNumber !== undefined) this.dependencies.selectProgram(firstProgramNumber);
            this.dependencies.reportTiming('program-generation', started, programs.length);
            this.dependencies.setStatus(`Generated ${programs.length} Programs`);
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, busy: false, error: message };
            this.dependencies.setStatus(message);
        }
    }
}

export function programNameError(name: string): string | null {
    if (name.length < 1 || name.length > 8) return 'Program names must contain 1-8 characters';
    if (name.trim() !== name) return 'Program names cannot begin or end with a space';
    for (const character of name) {
        const code = character.charCodeAt(0);
        if (code < 0x20 || code > 0x7e) return 'Program names must use printable ASCII characters';
    }
    return null;
}

function requestFromInspection(volumeName: string, inspection: ProgramGenerationInspection): ProgramGenerationRequest {
    const rows = inspection.candidates.map((candidate) => ({
        ...candidate,
        selected: candidate.defaultSelected,
        programName: candidate.defaultProgramName,
    }));
    return {
        volumeName,
        inspection,
        rows: packRows(rows, inspection.availableProgramNumbers),
        loading: false,
        busy: false,
        error: '',
    };
}

function packRows(rows: ProgramGenerationRow[], availableProgramNumbers: number[]): ProgramGenerationRow[] {
    let selectedIndex = 0;
    return rows.map((row) => {
        if (!row.selected || selectedIndex >= availableProgramNumbers.length) {
            return { ...row, selected: false, programNumber: null };
        }
        const programNumber = availableProgramNumbers[selectedIndex++];
        return { ...row, programNumber };
    });
}

function selectedPrograms(rows: ProgramGenerationRow[]): ProgramGenerationSelection[] {
    return rows.flatMap((row) =>
        row.selected && row.programNumber !== null
            ? [{ targetObjectId: row.targetObjectId, programNumber: row.programNumber, programName: row.programName }]
            : [],
    );
}

function inspectionFingerprint(inspection: ProgramGenerationInspection): string {
    return JSON.stringify({
        imageId: inspection.imageId,
        revision: inspection.revision,
        contentScopeId: inspection.contentScopeId,
        availableProgramNumbers: inspection.availableProgramNumbers,
        candidates: inspection.candidates.map((candidate) => ({
            targetObjectId: candidate.targetObjectId,
            targetObjectName: candidate.targetObjectName,
            targetObjectType: candidate.targetObjectType,
            defaultProgramName: candidate.defaultProgramName,
        })),
    });
}
