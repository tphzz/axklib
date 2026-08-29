import { HttpImageSessions } from './httpImageSessions';
import type {
    CompanionSelection,
    AllocationMapReference,
    ContentPage,
    ObjectPage,
    ObjectPageFilter,
    OpenedImage,
    ImageValidationIssue,
    ImageOpenOptions,
    RelationshipPage,
    RelationshipPageFilter,
    SystemProgramContexts,
    ProgramAssignmentCleanupInspection,
    ProgramAssignmentCleanupSelection,
    JobState,
} from './transport';
import type { ImageLocation } from './storageLocations';

export class HttpImageSessionReads {
    protected constructor(protected readonly imageSessions: HttpImageSessions) {}

    openImage(location: ImageLocation, options?: ImageOpenOptions): Promise<OpenedImage> {
        return this.imageSessions.open(location, options);
    }

    keepImageAlive(sessionId: number): Promise<void> {
        return this.imageSessions.keepAlive(sessionId);
    }

    refreshImage(sessionId: number): Promise<OpenedImage> {
        return this.imageSessions.refresh(sessionId);
    }

    attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage> {
        return this.imageSessions.attachCompanions(sessionId, selection);
    }

    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        return this.imageSessions.contentChildren(sessionId, parentId, offset, limit);
    }

    validationIssues(sessionId: number): Promise<ImageValidationIssue[]> {
        return this.imageSessions.validationIssues(sessionId);
    }

    objectPage(sessionId: number, offset: number, limit: number, filter: ObjectPageFilter = {}): Promise<ObjectPage> {
        return this.imageSessions.objectPage(sessionId, offset, limit, filter);
    }

    relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: RelationshipPageFilter = {},
    ): Promise<RelationshipPage> {
        return this.imageSessions.relationshipPage(sessionId, offset, limit, filter);
    }

    systemProgramContexts(sessionId: number, partitionIndex: number): Promise<SystemProgramContexts> {
        return this.imageSessions.systemProgramContexts(sessionId, partitionIndex);
    }

    allocationMapReference(sessionId: number): Promise<AllocationMapReference> {
        return this.imageSessions.allocationMapReference(sessionId);
    }

    inspectProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
    ): Promise<ProgramAssignmentCleanupInspection> {
        return this.imageSessions.inspectProgramAssignmentCleanup(sessionId, contentScopeId);
    }

    startProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
        assignments: ProgramAssignmentCleanupSelection[],
    ): Promise<JobState> {
        return this.imageSessions.startProgramAssignmentCleanup(sessionId, contentScopeId, assignments);
    }

    closeImage(sessionId: number): Promise<void> {
        return this.imageSessions.close(sessionId);
    }
}
