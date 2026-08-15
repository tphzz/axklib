import { HttpImageSessions } from './httpImageSessions';
import type {
    CompanionSelection,
    ContentPage,
    ObjectPage,
    ObjectPageFilter,
    OpenedImage,
    RelationshipPage,
    RelationshipPageFilter,
    SystemProgramContexts,
} from './transport';
import type { ImageLocation } from './storageLocations';

export class HttpImageSessionReads {
    protected constructor(protected readonly imageSessions: HttpImageSessions) {}

    openImage(location: ImageLocation): Promise<OpenedImage> {
        return this.imageSessions.open(location);
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

    closeImage(sessionId: number): Promise<void> {
        return this.imageSessions.close(sessionId);
    }
}
