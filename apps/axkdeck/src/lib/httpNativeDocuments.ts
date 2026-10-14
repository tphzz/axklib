import type { ApiDocumentSummary, ApiImageSummary, SessionState } from './httpTransportModels';
import { imageLocation } from './httpImageSources';

export function selectedDocument(session: SessionState): ApiDocumentSummary | null {
    return session.summary.documents.find((document) => document.id === session.documentId) ?? null;
}

export function documentTarget(session: SessionState): { imageId: string; documentId: string } {
    const document = selectedDocument(session);
    if (!document) throw new Error('Select a native document');
    return { imageId: session.remoteId, documentId: document.id };
}

export function documentPath(session: SessionState): string {
    const target = documentTarget(session);
    return `/images/${encodeURIComponent(target.imageId)}/documents/${encodeURIComponent(target.documentId)}`;
}

export function documentSession(
    summary: ApiImageSummary,
    selected?: string | null,
    sourceMemberId?: string,
): SessionState {
    const sources = summary.sources.map((member) => imageLocation(member.source));
    if (!sources.length) throw new Error('Workspace has no admitted sources');
    const document = summary.documents.find((item) => item.id === selected);
    if (selected && !document) throw new Error('Selected native document is no longer available');
    const activeSourceMemberId = sourceMemberId ?? document?.sourceMemberIds[0] ?? summary.sources[0].id;
    const index = summary.sources.findIndex((member) => member.id === activeSourceMemberId);
    if (index < 0) throw new Error('Selected image is no longer available');
    const documents = summary.documents.filter((item) => item.sourceMemberIds.includes(activeSourceMemberId));
    if (document && !documents.includes(document))
        throw new Error('Native document does not belong to the selected image');
    const documentId = selected === undefined ? (documents.length === 1 ? documents[0].id : null) : selected;
    return {
        remoteId: summary.imageId,
        revision: summary.revision,
        sources,
        source: sources[index],
        summary,
        activeSourceMemberId,
        documentId,
        contentCursors: new Map(),
        contentItems: new Map(),
        objectCursors: new Map(),
        relationshipCursors: new Map(),
    };
}

export function refreshedDocumentSession(summary: ApiImageSummary, previous: SessionState): SessionState {
    const sourceMemberId = summary.sources.some((member) => member.id === previous.activeSourceMemberId)
        ? previous.activeSourceMemberId
        : summary.sources[0]?.id;
    const selected = summary.documents.some(
        (document) => document.id === previous.documentId && document.sourceMemberIds.includes(sourceMemberId),
    )
        ? previous.documentId
        : undefined;
    return documentSession(summary, selected, sourceMemberId);
}
