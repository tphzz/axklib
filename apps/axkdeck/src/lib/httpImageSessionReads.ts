import { HttpImageSessions } from './httpImageSessions';
import type { FilesystemEdit, FilesystemPage, FilesystemQuery, FilesystemImportEntry } from './filesystem';
import type {
    FilesystemExportDestination,
    FilesystemExportInspection,
    FilesystemExportLayout,
} from './filesystemExport';
import type {
    CompanionSelection,
    AllocationMapReference,
    ContentPage,
    ObjectPage,
    ObjectPageFilter,
    ObjectDetail,
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
import type { ImageLocation, InputFileLocation } from './storageLocations';

export class HttpImageSessionReads {
    startSu700Import(request: import('./su700Import').Su700Request): Promise<JobState> {
        return this.imageSessions.startSu700Import(request);
    }
    startFilesystemImportInspection(
        sessionId: number,
        expectedRevision: number,
        parentEntryId: string,
        entries: FilesystemImportEntry[],
    ): Promise<JobState> {
        return this.imageSessions.startFilesystemImportInspection(sessionId, expectedRevision, parentEntryId, entries);
    }
    startFilesystemInputInspection(inputs: InputFileLocation[]): Promise<JobState> {
        return this.imageSessions.startFilesystemInputInspection(inputs);
    }
    inspectFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        layout: FilesystemExportLayout = 'SELECTED_ENTRIES',
    ): Promise<FilesystemExportInspection> {
        return this.imageSessions.inspectFilesystemExport(sessionId, expectedRevision, entryIds, layout);
    }
    startFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        destination: FilesystemExportDestination,
        layout: FilesystemExportLayout = 'SELECTED_ENTRIES',
    ): Promise<JobState> {
        return this.imageSessions.startFilesystemExport(sessionId, expectedRevision, entryIds, destination, layout);
    }
    filesystem(sessionId: number, query?: FilesystemQuery): Promise<FilesystemPage> {
        return this.imageSessions.filesystem(sessionId, query);
    }
    startFilesystemEdits(sessionId: number, expectedRevision: number, edits: FilesystemEdit[]): Promise<JobState> {
        return this.imageSessions.startFilesystemEdits(sessionId, expectedRevision, edits);
    }
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

    objectDetail(sessionId: number, objectId: string): Promise<ObjectDetail> {
        return this.imageSessions.objectDetail(sessionId, objectId);
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
