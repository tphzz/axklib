import { HttpImageSessionReads } from './httpImageSessionReads';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpPackageOperations } from './httpPackageOperations';
import type { FloppyPlanRequest } from './floppyImport';
import type { InputFileLocation } from './storageLocations';
import type {
    JobState,
    ImageSessionPackageImportDestination,
    ImageSessionPackageImportPlan,
    PackageRename,
    PackageProgramSlotAssignment,
    PackageOpaqueSequenceDecision,
} from './transport';

export class HttpPackageTransport extends HttpImageSessionReads {
    protected constructor(
        sessions: HttpImageSessions,
        protected readonly packages: HttpPackageOperations,
    ) {
        super(sessions);
    }
    startFloppyInspection(sources: InputFileLocation[]): Promise<JobState> {
        return this.packages.startFloppyInspection(sources);
    }
    releaseFloppyInspection(token: string): Promise<void> {
        return this.packages.releaseFloppyInspection(token);
    }
    planFloppyImport(sessionId: number, request: FloppyPlanRequest): Promise<ImageSessionPackageImportPlan> {
        return this.packages.planFloppyImport(sessionId, request);
    }
    startFloppyImport(token: string): Promise<JobState> {
        return this.packages.startFloppyImport(token);
    }
    planImagePackageImport(
        sessionId: number,
        sources: InputFileLocation[],
        destination: ImageSessionPackageImportDestination,
        renames: PackageRename[] = [],
        programSlotAssignments: PackageProgramSlotAssignment[] = [],
        replacePlanToken?: string,
        opaqueSequenceDecisions: PackageOpaqueSequenceDecision[] = [],
    ): Promise<ImageSessionPackageImportPlan> {
        return this.packages.planImageImport(
            sessionId,
            sources,
            destination,
            renames,
            programSlotAssignments,
            replacePlanToken,
            opaqueSequenceDecisions,
        );
    }
    releaseImagePackageImportPlan(planToken: string): Promise<void> {
        return this.packages.releaseImageImportPlan(planToken);
    }
    startImagePackageImport(planToken: string): Promise<JobState> {
        return this.packages.startImageImport(planToken);
    }
}
