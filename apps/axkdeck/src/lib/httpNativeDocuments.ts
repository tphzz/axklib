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

export function documentSession(summary: ApiImageSummary, selected?: string | null): SessionState {
    const sources = summary.sources.map((member) => imageLocation(member.source));
    if (!sources.length) throw new Error('Workspace has no admitted sources');
    const documentId =
        selected === undefined ? (summary.documents.length === 1 ? summary.documents[0].id : null) : selected;
    if (documentId && !summary.documents.some((document) => document.id === documentId))
        throw new Error('Selected native document is no longer available');
    return {
        remoteId: summary.imageId,
        revision: summary.revision,
        sources,
        source: sources[0],
        summary,
        documentId,
        contentCursors: new Map(),
        contentItems: new Map(),
        objectCursors: new Map(),
        relationshipCursors: new Map(),
    };
}

export function refreshedDocumentSession(summary: ApiImageSummary, previous: SessionState): SessionState {
    const selected = summary.documents.some((document) => document.id === previous.documentId)
        ? previous.documentId
        : null;
    return documentSession(summary, selected);
}
